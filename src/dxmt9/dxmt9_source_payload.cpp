#include "dxmt9_source_payload.hpp"

#include "dxmt9/copy_materialization_ledger.hpp"

#include <algorithm>
#include <stdexcept>

namespace dxmt9::core {
namespace {

bool directCapacityAvailable(std::size_t current, std::size_t checkpoint,
                             std::size_t allowance) noexcept {
  return current >= checkpoint && current - checkpoint < allowance;
}

constexpr bool isPowerOfTwo(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

bool checkedAdd(std::size_t lhs, std::size_t rhs,
                std::size_t& result) noexcept {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return false;
  }
  result = lhs + rhs;
  return true;
}

bool checkedMultiply(std::size_t lhs, std::size_t rhs,
                     std::size_t& result) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return false;
  }
  result = lhs * rhs;
  return true;
}

bool checkedAlignUp(std::size_t value, std::size_t alignment,
                    std::size_t& result) noexcept {
  if (!isPowerOfTwo(alignment)) {
    return false;
  }
  const std::size_t mask = alignment - 1;
  if (value > std::numeric_limits<std::size_t>::max() - mask) {
    return false;
  }
  result = (value + mask) & ~mask;
  return true;
}

template <typename T>
constexpr std::size_t typeSize() noexcept {
  return sizeof(T);
}

template <typename T>
constexpr std::size_t typeAlignment() noexcept {
  return alignof(T);
}

struct RegionShape {
  SourcePayloadRegion id;
  std::size_t count;
  std::size_t elementSize;
  std::size_t alignment;
};

}  // namespace

bool ArenaByteBuffer::bind(std::span<std::byte> memory,
                           std::size_t baseAlignment) noexcept {
  if (bound_ || size_ != 0 || !isPowerOfTwo(baseAlignment) ||
      (memory.size() != 0 &&
       (memory.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(memory.data()) % baseAlignment != 0))) {
    return false;
  }
  data_ = memory.data();
  capacity_ = memory.size();
  baseAlignment_ = baseAlignment;
  bound_ = true;
  return true;
}

bool ArenaByteBuffer::try_append(std::span<const std::byte> bytes,
                                 std::size_t alignment,
                                 std::size_t& offset) noexcept {
  if (!bound_ || sealed_ || !isPowerOfTwo(alignment) ||
      alignment > baseAlignment_) {
    return false;
  }

  std::size_t alignedOffset = 0;
  if (!checkedAlignUp(size_, alignment, alignedOffset) ||
      alignedOffset > capacity_ || bytes.size() > capacity_ - alignedOffset) {
    return false;
  }

  if (alignedOffset != size_) {
    std::memset(data_ + size_, 0, alignedOffset - size_);
  }
  if (!bytes.empty()) {
    auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
        dxmt9::core::CopyMaterializationOwner::Unix);
    std::optional<dxmt9::core::CopyMaterializationEvent> event;
    if (ledger) {
      event.emplace(ledger,
                    dxmt9::core::CopyMaterializationClass::ArenaByteCopy,
                    bytes.size());
    }
    std::memmove(data_ + alignedOffset, bytes.data(), bytes.size());
  }
  offset = alignedOffset;
  size_ = alignedOffset + bytes.size();
  return true;
}

std::optional<SourcePayloadLayout> makeSourcePayloadLayout(
    const SourcePayloadCapacity& c,
    std::size_t pageSize,
    std::size_t maxPages) noexcept {
  if (pageSize == 0 || maxPages == 0) {
    return std::nullopt;
  }

  const std::array shapes{
      RegionShape{SourcePayloadRegion::CommandHeaders, c.commandHeaders,
                  typeSize<MetalCommandHeader>(), typeAlignment<MetalCommandHeader>()},
      RegionShape{SourcePayloadRegion::DrawHotStates, c.drawHotStates,
                  typeSize<FlatDrawStateRecord>(), typeAlignment<FlatDrawStateRecord>()},
      RegionShape{SourcePayloadRegion::DrawShaderLayouts, c.drawShaderLayouts,
                  typeSize<DrawShaderLayoutContext>(), typeAlignment<DrawShaderLayoutContext>()},
      RegionShape{SourcePayloadRegion::DrawDebugSnapshots, c.drawDebugSnapshots,
                  typeSize<DrawDebugSnapshot>(), typeAlignment<DrawDebugSnapshot>()},
      RegionShape{SourcePayloadRegion::DrawPsoSubviews, c.drawPsoSubviews,
                  typeSize<DrawPsoSubview>(), typeAlignment<DrawPsoSubview>()},
      RegionShape{SourcePayloadRegion::DrawUniformFixedPayloads, c.drawUniformFixedPayloads,
                  typeSize<DrawUniformFixedPayloadRecord>(), typeAlignment<DrawUniformFixedPayloadRecord>()},
      RegionShape{SourcePayloadRegion::DrawUniformVertexConstants, c.drawUniformVertexConstants,
                  typeSize<DrawUniformVertexConstantsRecord>(), typeAlignment<DrawUniformVertexConstantsRecord>()},
      RegionShape{SourcePayloadRegion::DrawUniformVertexConstantBytes, c.drawUniformVertexConstantBytes,
                  typeSize<u8>(), kSourcePayloadByteAlignment},
      RegionShape{SourcePayloadRegion::DrawUniformPixelConstants, c.drawUniformPixelConstants,
                  typeSize<DrawUniformPixelConstantsRecord>(), typeAlignment<DrawUniformPixelConstantsRecord>()},
      RegionShape{SourcePayloadRegion::DrawUniformPixelConstantBytes, c.drawUniformPixelConstantBytes,
                  typeSize<u8>(), kSourcePayloadByteAlignment},
      RegionShape{SourcePayloadRegion::DrawUniformPayloads, c.drawUniformPayloads,
                  typeSize<DrawUniformPayloadRecord>(), typeAlignment<DrawUniformPayloadRecord>()},
      RegionShape{SourcePayloadRegion::DrawUniformPayloadLookupHeads, c.drawUniformPayloadLookupHeads,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformPayloadLookupTails, c.drawUniformPayloadLookupTails,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformPayloadLookupNext, c.drawUniformPayloadLookupNext,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformVertexConstantsLookupHeads, c.drawUniformVertexConstantsLookupHeads,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformVertexConstantsLookupTails, c.drawUniformVertexConstantsLookupTails,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformVertexConstantsLookupNext, c.drawUniformVertexConstantsLookupNext,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformPixelConstantsLookupHeads, c.drawUniformPixelConstantsLookupHeads,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformPixelConstantsLookupTails, c.drawUniformPixelConstantsLookupTails,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawUniformPixelConstantsLookupNext, c.drawUniformPixelConstantsLookupNext,
                  typeSize<std::uint32_t>(), typeAlignment<std::uint32_t>()},
      RegionShape{SourcePayloadRegion::DrawParams, c.drawParams,
                  typeSize<DrawParam>(), typeAlignment<DrawParam>()},
      RegionShape{SourcePayloadRegion::DrawPayloadBytes, c.drawPayloadBytes,
                  typeSize<u8>(), kSourcePayloadByteAlignment},
      RegionShape{SourcePayloadRegion::DrawRunRecords, c.drawRunRecords,
                  typeSize<DrawRunCommandRecord>(), typeAlignment<DrawRunCommandRecord>()},
      RegionShape{SourcePayloadRegion::ClearRecords, c.clearRecords,
                  typeSize<ArenaClearRecord>(), typeAlignment<ArenaClearRecord>()},
      RegionShape{SourcePayloadRegion::ClearRects, c.clearRects,
                  typeSize<Rect>(), typeAlignment<Rect>()},
      RegionShape{SourcePayloadRegion::SurfaceCopyRecords, c.surfaceCopyRecords,
                  typeSize<SurfaceCopyDesc>(), typeAlignment<SurfaceCopyDesc>()},
      RegionShape{SourcePayloadRegion::StretchRectRecords, c.stretchRectRecords,
                  typeSize<StretchRectDesc>(), typeAlignment<StretchRectDesc>()},
      RegionShape{SourcePayloadRegion::ReadbackRecords, c.readbackRecords,
                  typeSize<ReadbackDesc>(), typeAlignment<ReadbackDesc>()},
      RegionShape{SourcePayloadRegion::ColorFillRecords, c.colorFillRecords,
                  typeSize<ColorFillDesc>(), typeAlignment<ColorFillDesc>()},
      RegionShape{SourcePayloadRegion::DepthResolveRecords, c.depthResolveRecords,
                  typeSize<DepthResolveDesc>(), typeAlignment<DepthResolveDesc>()},
      RegionShape{SourcePayloadRegion::GenerateMipmapsRecords,
                  c.generateMipmapsRecords,
                  typeSize<GenerateMipmapsDesc>(),
                  typeAlignment<GenerateMipmapsDesc>()},
      RegionShape{SourcePayloadRegion::PresentRecords, c.presentRecords,
                  typeSize<PresentCommandRecord>(), typeAlignment<PresentCommandRecord>()},
  };
  static_assert(shapes.size() == kSourcePayloadRegionCount);

  SourcePayloadLayout layout{};
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < shapes.size(); ++i) {
    const auto& shape = shapes[i];
    std::size_t offset = 0;
    std::size_t byteCount = 0;
    std::size_t end = 0;
    if (shape.count > std::numeric_limits<std::uint32_t>::max() ||
        !checkedAlignUp(cursor, shape.alignment, offset) ||
        !checkedMultiply(shape.count, shape.elementSize, byteCount) ||
        !checkedAdd(offset, byteCount, end)) {
      return std::nullopt;
    }
    layout.regions[i] = SourcePayloadLayoutRegion{
        .id = shape.id,
        .offset = offset,
        .byteCount = byteCount,
        .alignment = shape.alignment,
        .elementCount = shape.count,
        .elementSize = shape.elementSize,
    };
    cursor = end;
    layout.maximumAlignment = std::max(layout.maximumAlignment, shape.alignment);
  }

  layout.requiredBaseAlignment = layout.maximumAlignment;
  if (pageSize % layout.requiredBaseAlignment != 0) {
    return std::nullopt;
  }

  layout.usedBytes = cursor;
  if (cursor == 0) {
    return layout;
  }
  if (cursor > std::numeric_limits<std::size_t>::max() - (pageSize - 1)) {
    return std::nullopt;
  }
  layout.pageCount = (cursor + pageSize - 1) / pageSize;
  if (layout.pageCount > std::numeric_limits<std::uint32_t>::max() ||
      layout.pageCount > maxPages) {
    return std::nullopt;
  }
  return layout;
}

std::optional<ArenaSourcePayloadLayout> makeArenaSourcePayloadLayout(
    std::span<const SourcePayloadLayout> segments,
    std::size_t pageSize,
    std::size_t maxPages) noexcept {
  if (segments.empty() ||
      segments.size() > kMaxArenaSourcePayloadSegments || pageSize == 0 ||
      maxPages == 0) {
    return std::nullopt;
  }

  ArenaSourcePayloadLayout result{};
  std::size_t cursor = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& segment = segments[i];
    std::size_t offset = 0;
    std::size_t end = 0;
    if (segment.usedBytes == 0 || segment.pageCount == 0 ||
        !isPowerOfTwo(segment.requiredBaseAlignment) ||
        pageSize % segment.requiredBaseAlignment != 0 ||
        !checkedAlignUp(cursor, segment.requiredBaseAlignment, offset) ||
        !checkedAdd(offset, segment.usedBytes, end)) {
      return std::nullopt;
    }
    result.segments[i] = ArenaSourcePayloadSegmentLayout{
        .layout = segment,
        .byteOffset = offset,
    };
    cursor = end;
  }
  if (cursor > std::numeric_limits<std::size_t>::max() - (pageSize - 1)) {
    return std::nullopt;
  }
  const std::size_t pages = (cursor + pageSize - 1) / pageSize;
  if (pages == 0 || pages > maxPages ||
      pages > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  result.segmentCount = segments.size();
  result.usedBytes = cursor;
  result.pageCount = pages;
  return result;
}

namespace {

bool preflightRegion(std::span<std::byte> memory,
                     const SourcePayloadLayout& layout,
                     SourcePayloadRegion id,
                     std::size_t elementSize,
                     std::size_t alignment) noexcept {
  const auto& region = layout.region(id);
  std::size_t expectedByteCount = 0;
  if (region.id != id || region.elementSize != elementSize ||
      region.alignment != alignment ||
      region.elementCount > std::numeric_limits<std::uint32_t>::max() ||
      !checkedMultiply(region.elementCount, elementSize, expectedByteCount) ||
      region.byteCount != expectedByteCount ||
      region.offset % alignment != 0 ||
      region.offset > layout.usedBytes ||
      region.byteCount > layout.usedBytes - region.offset ||
      region.offset > memory.size() ||
      region.byteCount > memory.size() - region.offset ||
      (region.byteCount != 0 &&
       reinterpret_cast<std::uintptr_t>(memory.data() + region.offset) %
               alignment !=
           0)) {
    return false;
  }
  return true;
}

bool preflightLayout(std::span<std::byte> memory,
                     const SourcePayloadLayout& layout) noexcept {
  if (!isPowerOfTwo(layout.requiredBaseAlignment) ||
      layout.requiredBaseAlignment != layout.maximumAlignment ||
      layout.usedBytes > memory.size() ||
      layout.pageCount > std::numeric_limits<std::uint32_t>::max() ||
      (layout.usedBytes == 0) != (layout.pageCount == 0) ||
      (layout.usedBytes != 0 &&
       (memory.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(memory.data()) %
                layout.requiredBaseAlignment !=
            0))) {
    return false;
  }

  std::size_t cursor = 0;
  std::size_t maximumAlignment = 1;
  for (std::size_t i = 0; i < layout.regions.size(); ++i) {
    const auto& region = layout.regions[i];
    std::size_t end = 0;
    if (region.id != static_cast<SourcePayloadRegion>(i) ||
        !isPowerOfTwo(region.alignment) ||
        region.offset < cursor || region.offset % region.alignment != 0 ||
        !checkedAdd(region.offset, region.byteCount, end) ||
        end > layout.usedBytes || end > memory.size()) {
      return false;
    }
    cursor = end;
    maximumAlignment = std::max(maximumAlignment, region.alignment);
  }
  if (cursor != layout.usedBytes ||
      maximumAlignment != layout.maximumAlignment) {
    return false;
  }

#define DXMT9_PREFLIGHT_REGION(id, type, alignment)                         \
  preflightRegion(memory, layout, SourcePayloadRegion::id, sizeof(type),   \
                  alignment)
  return
      DXMT9_PREFLIGHT_REGION(CommandHeaders, MetalCommandHeader,
                             alignof(MetalCommandHeader)) &&
      DXMT9_PREFLIGHT_REGION(DrawHotStates, FlatDrawStateRecord,
                             alignof(FlatDrawStateRecord)) &&
      DXMT9_PREFLIGHT_REGION(DrawShaderLayouts, DrawShaderLayoutContext,
                             alignof(DrawShaderLayoutContext)) &&
      DXMT9_PREFLIGHT_REGION(DrawDebugSnapshots, DrawDebugSnapshot,
                             alignof(DrawDebugSnapshot)) &&
      DXMT9_PREFLIGHT_REGION(DrawPsoSubviews, DrawPsoSubview,
                             alignof(DrawPsoSubview)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformFixedPayloads,
                             DrawUniformFixedPayloadRecord,
                             alignof(DrawUniformFixedPayloadRecord)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformVertexConstants,
                             DrawUniformVertexConstantsRecord,
                             alignof(DrawUniformVertexConstantsRecord)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformVertexConstantBytes, u8,
                             kSourcePayloadByteAlignment) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPixelConstants,
                             DrawUniformPixelConstantsRecord,
                             alignof(DrawUniformPixelConstantsRecord)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPixelConstantBytes, u8,
                             kSourcePayloadByteAlignment) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPayloads, DrawUniformPayloadRecord,
                             alignof(DrawUniformPayloadRecord)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPayloadLookupHeads, std::uint32_t,
                             alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPayloadLookupTails, std::uint32_t,
                             alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPayloadLookupNext, std::uint32_t,
                             alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformVertexConstantsLookupHeads,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformVertexConstantsLookupTails,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformVertexConstantsLookupNext,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPixelConstantsLookupHeads,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPixelConstantsLookupTails,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawUniformPixelConstantsLookupNext,
                             std::uint32_t, alignof(std::uint32_t)) &&
      DXMT9_PREFLIGHT_REGION(DrawParams, DrawParam, alignof(DrawParam)) &&
      DXMT9_PREFLIGHT_REGION(DrawPayloadBytes, u8,
                             kSourcePayloadByteAlignment) &&
      DXMT9_PREFLIGHT_REGION(DrawRunRecords, DrawRunCommandRecord,
                             alignof(DrawRunCommandRecord)) &&
      DXMT9_PREFLIGHT_REGION(ClearRecords, ArenaClearRecord,
                             alignof(ArenaClearRecord)) &&
      DXMT9_PREFLIGHT_REGION(ClearRects, Rect, alignof(Rect)) &&
      DXMT9_PREFLIGHT_REGION(SurfaceCopyRecords, SurfaceCopyDesc,
                             alignof(SurfaceCopyDesc)) &&
      DXMT9_PREFLIGHT_REGION(StretchRectRecords, StretchRectDesc,
                             alignof(StretchRectDesc)) &&
      DXMT9_PREFLIGHT_REGION(ReadbackRecords, ReadbackDesc,
                             alignof(ReadbackDesc)) &&
      DXMT9_PREFLIGHT_REGION(ColorFillRecords, ColorFillDesc,
                             alignof(ColorFillDesc)) &&
      DXMT9_PREFLIGHT_REGION(DepthResolveRecords, DepthResolveDesc,
                             alignof(DepthResolveDesc)) &&
      DXMT9_PREFLIGHT_REGION(PresentRecords, PresentCommandRecord,
                             alignof(PresentCommandRecord));
#undef DXMT9_PREFLIGHT_REGION
}

template <typename T>
bool bindRegion(ArenaSoA<T>& storage,
                std::span<std::byte> memory,
                const SourcePayloadLayout& layout,
                SourcePayloadRegion id) noexcept {
  const auto& region = layout.region(id);
  return storage.bind(memory.subspan(region.offset, region.byteCount),
                      region.elementCount);
}

bool bindByteRegion(ArenaByteBuffer& storage,
                    std::span<std::byte> memory,
                    const SourcePayloadLayout& layout,
                    SourcePayloadRegion id) noexcept {
  const auto& region = layout.region(id);
  return storage.bind(memory.subspan(region.offset, region.byteCount),
                      kSourcePayloadByteAlignment);
}

std::span<const u8> asU8(std::span<const std::byte> bytes) noexcept {
  return {reinterpret_cast<const u8*>(bytes.data()), bytes.size()};
}

ClearCommandView makeClearView(const ClearDesc& clear) noexcept {
  return ClearCommandView{
      .colorAttachments = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
      .clearColor = clear.clearColor,
      .clearDepth = clear.clearDepth,
      .clearStencil = clear.clearStencil,
      .color = clear.color,
      .depth = clear.depth,
      .stencil = clear.stencil,
      .rects = clear.rects,
  };
}

std::optional<ClearCommandView> makeClearView(
    const ArenaClearRecord& clear,
    std::span<const Rect> rects) noexcept {
  if (clear.firstRect > rects.size() ||
      clear.rectCount > rects.size() - clear.firstRect) {
    return std::nullopt;
  }
  return ClearCommandView{
      .colorAttachments = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
      .clearColor = clear.clearColor,
      .clearDepth = clear.clearDepth,
      .clearStencil = clear.clearStencil,
      .color = clear.color,
      .depth = clear.depth,
      .stencil = clear.stencil,
      .rects = rects.subspan(clear.firstRect, clear.rectCount),
  };
}

}  // namespace

bool ArenaSourcePayloadBlock::bind(
    std::span<std::byte> memory,
    const SourcePayloadLayout& layout) noexcept {
  // Validate the complete externally supplied layout before binding any PMR
  // resource. A malformed late region must leave this block pristine and
  // eligible for a later bind with a valid layout.
  if (bound_ || !preflightLayout(memory, layout)) {
    return false;
  }

  const bool ok =
      bindRegion(commandHeaders_, memory, layout,
                 SourcePayloadRegion::CommandHeaders) &&
      bindRegion(drawHotStates_, memory, layout,
                 SourcePayloadRegion::DrawHotStates) &&
      bindRegion(drawShaderLayouts_, memory, layout,
                 SourcePayloadRegion::DrawShaderLayouts) &&
      bindRegion(drawDebugSnapshots_, memory, layout,
                 SourcePayloadRegion::DrawDebugSnapshots) &&
      bindRegion(drawPsoSubviews_, memory, layout,
                 SourcePayloadRegion::DrawPsoSubviews) &&
      bindRegion(drawUniformFixedPayloads_, memory, layout,
                 SourcePayloadRegion::DrawUniformFixedPayloads) &&
      bindRegion(drawUniformVertexConstants_, memory, layout,
                 SourcePayloadRegion::DrawUniformVertexConstants) &&
      bindByteRegion(drawUniformVertexConstantBytes_, memory, layout,
                     SourcePayloadRegion::DrawUniformVertexConstantBytes) &&
      bindRegion(drawUniformPixelConstants_, memory, layout,
                 SourcePayloadRegion::DrawUniformPixelConstants) &&
      bindByteRegion(drawUniformPixelConstantBytes_, memory, layout,
                     SourcePayloadRegion::DrawUniformPixelConstantBytes) &&
      bindRegion(drawUniformPayloads_, memory, layout,
                 SourcePayloadRegion::DrawUniformPayloads) &&
      bindRegion(drawUniformPayloadLookupHeads_, memory, layout,
                 SourcePayloadRegion::DrawUniformPayloadLookupHeads) &&
      bindRegion(drawUniformPayloadLookupTails_, memory, layout,
                 SourcePayloadRegion::DrawUniformPayloadLookupTails) &&
      bindRegion(drawUniformPayloadLookupNext_, memory, layout,
                 SourcePayloadRegion::DrawUniformPayloadLookupNext) &&
      bindRegion(drawUniformVertexConstantsLookupHeads_, memory, layout,
                 SourcePayloadRegion::DrawUniformVertexConstantsLookupHeads) &&
      bindRegion(drawUniformVertexConstantsLookupTails_, memory, layout,
                 SourcePayloadRegion::DrawUniformVertexConstantsLookupTails) &&
      bindRegion(drawUniformVertexConstantsLookupNext_, memory, layout,
                 SourcePayloadRegion::DrawUniformVertexConstantsLookupNext) &&
      bindRegion(drawUniformPixelConstantsLookupHeads_, memory, layout,
                 SourcePayloadRegion::DrawUniformPixelConstantsLookupHeads) &&
      bindRegion(drawUniformPixelConstantsLookupTails_, memory, layout,
                 SourcePayloadRegion::DrawUniformPixelConstantsLookupTails) &&
      bindRegion(drawUniformPixelConstantsLookupNext_, memory, layout,
                 SourcePayloadRegion::DrawUniformPixelConstantsLookupNext) &&
      bindRegion(drawParams_, memory, layout,
                 SourcePayloadRegion::DrawParams) &&
      bindByteRegion(drawPayloadBytes_, memory, layout,
                     SourcePayloadRegion::DrawPayloadBytes) &&
      bindRegion(drawRunRecords_, memory, layout,
                 SourcePayloadRegion::DrawRunRecords) &&
      bindRegion(clearRecords_, memory, layout,
                 SourcePayloadRegion::ClearRecords) &&
      bindRegion(clearRects_, memory, layout,
                 SourcePayloadRegion::ClearRects) &&
      bindRegion(surfaceCopyRecords_, memory, layout,
                 SourcePayloadRegion::SurfaceCopyRecords) &&
      bindRegion(stretchRectRecords_, memory, layout,
                 SourcePayloadRegion::StretchRectRecords) &&
      bindRegion(readbackRecords_, memory, layout,
                 SourcePayloadRegion::ReadbackRecords) &&
      bindRegion(colorFillRecords_, memory, layout,
                 SourcePayloadRegion::ColorFillRecords) &&
      bindRegion(depthResolveRecords_, memory, layout,
                 SourcePayloadRegion::DepthResolveRecords) &&
      bindRegion(generateMipmapsRecords_, memory, layout,
                 SourcePayloadRegion::GenerateMipmapsRecords) &&
      bindRegion(presentRecords_, memory, layout,
                 SourcePayloadRegion::PresentRecords);
  bound_ = ok;
  if (ok) {
    boundBase_ = memory.data();
    boundExtent_ = memory.size();
  }
  return ok;
}

void ArenaSourcePayloadBlock::publishCounts() noexcept {
  actualCounts_ = {
      commandHeaders_.size(),
      drawHotStates_.size(),
      drawShaderLayouts_.size(),
      drawDebugSnapshots_.size(),
      drawPsoSubviews_.size(),
      drawUniformFixedPayloads_.size(),
      drawUniformVertexConstants_.size(),
      drawUniformVertexConstantBytes_.size(),
      drawUniformPixelConstants_.size(),
      drawUniformPixelConstantBytes_.size(),
      drawUniformPayloads_.size(),
      drawUniformPayloadLookupHeads_.size(),
      drawUniformPayloadLookupTails_.size(),
      drawUniformPayloadLookupNext_.size(),
      drawUniformVertexConstantsLookupHeads_.size(),
      drawUniformVertexConstantsLookupTails_.size(),
      drawUniformVertexConstantsLookupNext_.size(),
      drawUniformPixelConstantsLookupHeads_.size(),
      drawUniformPixelConstantsLookupTails_.size(),
      drawUniformPixelConstantsLookupNext_.size(),
      drawParams_.size(),
      drawPayloadBytes_.size(),
      drawRunRecords_.size(),
      clearRecords_.size(),
      clearRects_.size(),
      surfaceCopyRecords_.size(),
      stretchRectRecords_.size(),
      readbackRecords_.size(),
      colorFillRecords_.size(),
      depthResolveRecords_.size(),
      generateMipmapsRecords_.size(),
      presentRecords_.size(),
  };

  commandHeaders_.seal();
  drawHotStates_.seal();
  drawShaderLayouts_.seal();
  drawDebugSnapshots_.seal();
  drawPsoSubviews_.seal();
  drawUniformFixedPayloads_.seal();
  drawUniformVertexConstants_.seal();
  drawUniformVertexConstantBytes_.seal();
  drawUniformPixelConstants_.seal();
  drawUniformPixelConstantBytes_.seal();
  drawUniformPayloads_.seal();
  drawUniformPayloadLookupHeads_.seal();
  drawUniformPayloadLookupTails_.seal();
  drawUniformPayloadLookupNext_.seal();
  drawUniformVertexConstantsLookupHeads_.seal();
  drawUniformVertexConstantsLookupTails_.seal();
  drawUniformVertexConstantsLookupNext_.seal();
  drawUniformPixelConstantsLookupHeads_.seal();
  drawUniformPixelConstantsLookupTails_.seal();
  drawUniformPixelConstantsLookupNext_.seal();
  drawParams_.seal();
  drawPayloadBytes_.seal();
  drawRunRecords_.seal();
  clearRecords_.seal();
  clearRects_.seal();
  surfaceCopyRecords_.seal();
  stretchRectRecords_.seal();
  readbackRecords_.seal();
  colorFillRecords_.seal();
  depthResolveRecords_.seal();
  generateMipmapsRecords_.seal();
  presentRecords_.seal();
  published_ = true;
}

bool ArenaSourcePayloadBlock::validateForPublish() const noexcept {
  if (!bound_ || published_ || commandHeaders_.empty() ||
      drawHotStates_.size() != drawShaderLayouts_.size() ||
      drawHotStates_.size() != drawDebugSnapshots_.size()) {
    return false;
  }

  const auto uniformRecordValid = [&](DrawUniformHandle handle) noexcept {
    return !handle.valid() ||
           (handle.index < drawUniformPayloads_.size() &&
            drawUniformPayloads_[handle.index].handle == handle);
  };
  const auto fixedRecordValid = [&](DrawUniformFixedHandle handle) noexcept {
    return !handle.valid() ||
           (handle.index < drawUniformFixedPayloads_.size() &&
            drawUniformFixedPayloads_[handle.index].handle == handle);
  };
  const auto vertexRecordValid = [&](DrawUniformStageHandle handle) noexcept {
    return !handle.valid() ||
           (handle.index < drawUniformVertexConstants_.size() &&
            drawUniformVertexConstants_[handle.index].handle == handle);
  };
  const auto pixelRecordValid = [&](DrawUniformStageHandle handle) noexcept {
    return !handle.valid() ||
           (handle.index < drawUniformPixelConstants_.size() &&
            drawUniformPixelConstants_[handle.index].handle == handle);
  };
  const auto lookupValid = [](
      std::size_t recordCount,
      std::span<const std::uint32_t> heads,
      std::span<const std::uint32_t> tails,
      std::span<const std::uint32_t> next) noexcept {
    if (heads.empty() && tails.empty() && next.empty()) {
      return true;
    }
    if (heads.empty() || heads.size() != tails.size() ||
        next.size() != recordCount) {
      return false;
    }
    const auto indexValid = [recordCount](std::uint32_t index) noexcept {
      return index == detail::kChunkSlotInvalidUniformIndex ||
             index < recordCount;
    };
    return std::all_of(heads.begin(), heads.end(), indexValid) &&
           std::all_of(tails.begin(), tails.end(), indexValid) &&
           std::all_of(next.begin(), next.end(), indexValid);
  };

  if (!lookupValid(drawUniformPayloads_.size(),
                   drawUniformPayloadLookupHeads_.span(),
                   drawUniformPayloadLookupTails_.span(),
                   drawUniformPayloadLookupNext_.span()) ||
      !lookupValid(drawUniformVertexConstants_.size(),
                   drawUniformVertexConstantsLookupHeads_.span(),
                   drawUniformVertexConstantsLookupTails_.span(),
                   drawUniformVertexConstantsLookupNext_.span()) ||
      !lookupValid(drawUniformPixelConstants_.size(),
                   drawUniformPixelConstantsLookupHeads_.span(),
                   drawUniformPixelConstantsLookupTails_.span(),
                   drawUniformPixelConstantsLookupNext_.span())) {
    return false;
  }

  for (const auto& uniform : drawUniformPayloads_.span()) {
    if (!fixedRecordValid(uniform.fixedHandle) ||
        !vertexRecordValid(uniform.vertexConstantsHandle) ||
        !pixelRecordValid(uniform.pixelConstantsHandle)) {
      return false;
    }
  }
  for (const auto& record : drawUniformVertexConstants_.span()) {
    if (record.constants.byteOffset > drawUniformVertexConstantBytes_.size() ||
        record.constants.byteSize >
            drawUniformVertexConstantBytes_.size() - record.constants.byteOffset) {
      return false;
    }
  }
  for (const auto& record : drawUniformPixelConstants_.span()) {
    if (record.constants.byteOffset > drawUniformPixelConstantBytes_.size() ||
        record.constants.byteSize >
            drawUniformPixelConstantBytes_.size() - record.constants.byteOffset) {
      return false;
    }
  }

  std::size_t expectedDrawRun = 0;
  std::size_t expectedClear = 0;
  std::size_t expectedSurfaceCopy = 0;
  std::size_t expectedStretchRect = 0;
  std::size_t expectedReadback = 0;
  std::size_t expectedColorFill = 0;
  std::size_t expectedDepthResolve = 0;
  std::size_t expectedGenerateMipmaps = 0;
  std::size_t expectedPresent = 0;
  std::size_t clearRectCursor = 0;
  for (const auto& header : commandHeaders_.span()) {
    const auto payloadIndex = header.payloadIndex.value;
    switch (header.kind) {
    case MetalCommandKind::DrawRun: {
      if (payloadIndex != expectedDrawRun ||
          payloadIndex >= drawRunRecords_.size() ||
          payloadIndex >= drawPsoSubviews_.size()) {
        return false;
      }
      ++expectedDrawRun;
      const auto& record = drawRunRecords_[payloadIndex];
      if (record.stateIndex >= drawHotStates_.size() ||
          record.firstParam > drawParams_.size() ||
          record.paramCount > drawParams_.size() - record.firstParam ||
          record.payloadOffset > drawPayloadBytes_.size() ||
          record.payloadSize > drawPayloadBytes_.size() - record.payloadOffset ||
          !uniformRecordValid(record.uniformHandle)) {
        return false;
      }
      for (std::size_t i = 0; i < record.paramCount; ++i) {
        const auto& param = drawParams_[record.firstParam + i];
        const auto rangeFits = [&](DrawPayloadRange range) noexcept {
          return range.offset <= record.payloadSize &&
                 range.size <= record.payloadSize - range.offset;
        };
        if (!uniformRecordValid(param.uniformHandle) ||
            !rangeFits(param.userVertexRange) ||
            !rangeFits(param.userIndexRange) ||
            !rangeFits(param.bindingOverrideRange) ||
            !rangeFits(param.bindingSnapshotRange)) {
          return false;
        }
      }
      break;
    }
    case MetalCommandKind::Clear: {
      if (payloadIndex != expectedClear ||
          payloadIndex >= clearRecords_.size()) {
        return false;
      }
      ++expectedClear;
      if (const auto& clear = clearRecords_[payloadIndex];
          clear.firstRect != clearRectCursor ||
          clear.firstRect > clearRects_.size() ||
          clear.rectCount > clearRects_.size() - clear.firstRect) {
        return false;
      } else {
        clearRectCursor += clear.rectCount;
      }
      break;
    }
    case MetalCommandKind::SurfaceCopy:
      if (payloadIndex != expectedSurfaceCopy ||
          payloadIndex >= surfaceCopyRecords_.size()) return false;
      ++expectedSurfaceCopy;
      break;
    case MetalCommandKind::StretchRect:
      if (payloadIndex != expectedStretchRect ||
          payloadIndex >= stretchRectRecords_.size()) return false;
      ++expectedStretchRect;
      break;
    case MetalCommandKind::Readback:
      if (payloadIndex != expectedReadback ||
          payloadIndex >= readbackRecords_.size()) return false;
      ++expectedReadback;
      break;
    case MetalCommandKind::ColorFill:
      if (payloadIndex != expectedColorFill ||
          payloadIndex >= colorFillRecords_.size()) return false;
      ++expectedColorFill;
      break;
    case MetalCommandKind::DepthResolve:
      if (payloadIndex != expectedDepthResolve ||
          payloadIndex >= depthResolveRecords_.size()) return false;
      ++expectedDepthResolve;
      break;
    case MetalCommandKind::GenerateMipmaps:
      if (payloadIndex != expectedGenerateMipmaps ||
          payloadIndex >= generateMipmapsRecords_.size()) return false;
      ++expectedGenerateMipmaps;
      break;
    case MetalCommandKind::Present:
      if (payloadIndex != expectedPresent ||
          payloadIndex >= presentRecords_.size()) return false;
      ++expectedPresent;
      break;
    default:
      return false;
    }
  }
  return expectedDrawRun == drawRunRecords_.size() &&
         expectedDrawRun == drawPsoSubviews_.size() &&
         expectedClear == clearRecords_.size() &&
         clearRectCursor == clearRects_.size() &&
         expectedSurfaceCopy == surfaceCopyRecords_.size() &&
         expectedStretchRect == stretchRectRecords_.size() &&
         expectedReadback == readbackRecords_.size() &&
         expectedColorFill == colorFillRecords_.size() &&
         expectedDepthResolve == depthResolveRecords_.size() &&
         expectedGenerateMipmaps == generateMipmapsRecords_.size() &&
         expectedPresent == presentRecords_.size();
}

void ArenaSourcePayloadBlock::destroyConstructed() noexcept {
  presentRecords_.destroyConstructed();
  generateMipmapsRecords_.destroyConstructed();
  depthResolveRecords_.destroyConstructed();
  colorFillRecords_.destroyConstructed();
  readbackRecords_.destroyConstructed();
  stretchRectRecords_.destroyConstructed();
  surfaceCopyRecords_.destroyConstructed();
  clearRects_.destroyConstructed();
  clearRecords_.destroyConstructed();
  drawRunRecords_.destroyConstructed();
  drawPayloadBytes_.discard();
  drawParams_.destroyConstructed();
  drawUniformPixelConstantsLookupNext_.destroyConstructed();
  drawUniformPixelConstantsLookupTails_.destroyConstructed();
  drawUniformPixelConstantsLookupHeads_.destroyConstructed();
  drawUniformVertexConstantsLookupNext_.destroyConstructed();
  drawUniformVertexConstantsLookupTails_.destroyConstructed();
  drawUniformVertexConstantsLookupHeads_.destroyConstructed();
  drawUniformPayloadLookupNext_.destroyConstructed();
  drawUniformPayloadLookupTails_.destroyConstructed();
  drawUniformPayloadLookupHeads_.destroyConstructed();
  drawUniformPayloads_.destroyConstructed();
  drawUniformPixelConstantBytes_.discard();
  drawUniformPixelConstants_.destroyConstructed();
  drawUniformVertexConstantBytes_.discard();
  drawUniformVertexConstants_.destroyConstructed();
  drawUniformFixedPayloads_.destroyConstructed();
  drawPsoSubviews_.destroyConstructed();
  drawDebugSnapshots_.destroyConstructed();
  drawShaderLayouts_.destroyConstructed();
  drawHotStates_.destroyConstructed();
  commandHeaders_.destroyConstructed();
  published_ = false;
  destroyed_ = true;
}

ArenaSourcePayloadBuilder::ArenaSourcePayloadBuilder(
    ArenaSourcePayloadBlock& block,
    const SourcePayloadLayout& layout,
    std::span<std::byte> memory) noexcept
    : block_(&block), good_(block.bind(memory, layout)) {}

bool ArenaSourcePayloadBuilder::publish() noexcept {
  if (!good_ || !block_ || !block_->validateForPublish()) {
    good_ = false;
    return false;
  }
  block_->publishCounts();
  return true;
}

bool ArenaSourcePayloadBuilder::publishValidationReady() const noexcept {
  return good_ && block_ && block_->validateForPublish();
}

void ArenaSourcePayloadBuilder::rollback() noexcept {
  if (block_) {
    block_->destroyConstructed();
  }
  good_ = false;
}

bool ArenaSourcePayloadBuilder::tryAppendCommand(
    MetalCommandKind kind,
    std::uint32_t payloadIndex) noexcept {
  return append(block_->commandHeaders_, MetalCommandHeader{
      .kind = kind,
      .payloadIndex = CommandPayloadIndex::fromU32(payloadIndex),
  });
}

bool ArenaSourcePayloadBuilder::tryAppendDrawHotState(
    const FlatDrawStateRecord& value) noexcept {
  return append(block_->drawHotStates_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawShaderLayout(
    DrawShaderLayoutContext&& value) noexcept {
  if (!good_ || !block_->drawShaderLayouts_.try_emplace_back(std::move(value))) {
    good_ = false;
    return false;
  }
  return true;
}

bool ArenaSourcePayloadBuilder::tryAppendDrawShaderLayout(
    const DrawShaderLayoutContext& value) noexcept {
  if (!good_ || !block_->drawShaderLayouts_.try_copy_back(value)) {
    good_ = false;
    return false;
  }
  return true;
}

const FlatDrawStateRecord* ArenaSourcePayloadBuilder::drawHotState(
    std::size_t index) const noexcept {
  return good_ && block_ && index < block_->drawHotStates_.size()
      ? &block_->drawHotStates_[index]
      : nullptr;
}

const DrawShaderLayoutContext* ArenaSourcePayloadBuilder::drawShaderLayout(
    std::size_t index) const noexcept {
  return good_ && block_ && index < block_->drawShaderLayouts_.size()
      ? &block_->drawShaderLayouts_[index]
      : nullptr;
}

DrawRunCommandRecord* ArenaSourcePayloadBuilder::drawRun(
    std::size_t index) noexcept {
  return good_ && block_ && index < block_->drawRunRecords_.size()
      ? &block_->drawRunRecords_[index]
      : nullptr;
}

bool ArenaSourcePayloadBuilder::tryAppendDrawDebugSnapshot(
    const DrawDebugSnapshot& value) noexcept {
  return append(block_->drawDebugSnapshots_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawPsoSubview(
    const DrawPsoSubview& value) noexcept {
  return append(block_->drawPsoSubviews_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawUniformFixedPayload(
    const DrawUniformFixedPayloadRecord& value) noexcept {
  return append(block_->drawUniformFixedPayloads_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawUniformVertexConstants(
    const DrawUniformVertexConstantsRecord& value) noexcept {
  return append(block_->drawUniformVertexConstants_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawUniformPixelConstants(
    const DrawUniformPixelConstantsRecord& value) noexcept {
  return append(block_->drawUniformPixelConstants_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawUniformPayload(
    const DrawUniformPayloadRecord& value) noexcept {
  return append(block_->drawUniformPayloads_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendLookup(
    SourcePayloadRegion region,
    std::uint32_t value) noexcept {
  if (!good_) {
    return false;
  }
  switch (region) {
  case SourcePayloadRegion::DrawUniformPayloadLookupHeads:
    return append(block_->drawUniformPayloadLookupHeads_, value);
  case SourcePayloadRegion::DrawUniformPayloadLookupTails:
    return append(block_->drawUniformPayloadLookupTails_, value);
  case SourcePayloadRegion::DrawUniformPayloadLookupNext:
    return append(block_->drawUniformPayloadLookupNext_, value);
  case SourcePayloadRegion::DrawUniformVertexConstantsLookupHeads:
    return append(block_->drawUniformVertexConstantsLookupHeads_, value);
  case SourcePayloadRegion::DrawUniformVertexConstantsLookupTails:
    return append(block_->drawUniformVertexConstantsLookupTails_, value);
  case SourcePayloadRegion::DrawUniformVertexConstantsLookupNext:
    return append(block_->drawUniformVertexConstantsLookupNext_, value);
  case SourcePayloadRegion::DrawUniformPixelConstantsLookupHeads:
    return append(block_->drawUniformPixelConstantsLookupHeads_, value);
  case SourcePayloadRegion::DrawUniformPixelConstantsLookupTails:
    return append(block_->drawUniformPixelConstantsLookupTails_, value);
  case SourcePayloadRegion::DrawUniformPixelConstantsLookupNext:
    return append(block_->drawUniformPixelConstantsLookupNext_, value);
  default:
    good_ = false;
    return false;
  }
}

bool ArenaSourcePayloadBuilder::tryAppendDrawParam(
    const DrawParam& value) noexcept {
  return append(block_->drawParams_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawRun(
    const DrawRunCommandRecord& value) noexcept {
  return append(block_->drawRunRecords_, value);
}

bool ArenaSourcePayloadBuilder::appendBytes(
    ArenaByteBuffer& storage,
    std::span<const u8> bytes,
    std::size_t alignment,
    std::size_t& offset) noexcept {
  if (!good_ ||
      !storage.try_append(std::as_bytes(bytes), alignment, offset)) {
    good_ = false;
    return false;
  }
  if (!bytes.empty()) {
  if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
          dxmt9::core::CopyMaterializationOwner::Unix)) {
      ledger->recordMaterialization(
          dxmt9::core::CopyMaterializationClass::QueueFinalSlotAppend,
          bytes.size());
    }
  }
  return true;
}

bool ArenaSourcePayloadBuilder::tryAppendVertexConstantBytes(
    std::span<const u8> bytes,
    std::size_t alignment,
    std::size_t& offset) noexcept {
  return appendBytes(block_->drawUniformVertexConstantBytes_, bytes,
                     alignment, offset);
}

bool ArenaSourcePayloadBuilder::tryAppendPixelConstantBytes(
    std::span<const u8> bytes,
    std::size_t alignment,
    std::size_t& offset) noexcept {
  return appendBytes(block_->drawUniformPixelConstantBytes_, bytes,
                     alignment, offset);
}

bool ArenaSourcePayloadBuilder::tryAppendDrawPayloadBytes(
    std::span<const u8> bytes,
    std::size_t alignment,
    std::size_t& offset) noexcept {
  return appendBytes(block_->drawPayloadBytes_, bytes, alignment, offset);
}

template <typename T>
bool ArenaSourcePayloadBuilder::appendCommandRecord(
    MetalCommandKind kind,
    ArenaSoA<T>& storage,
    const T& value) noexcept {
  if (!good_ || storage.remaining() == 0 ||
      block_->commandHeaders_.remaining() == 0 ||
      storage.size() > std::numeric_limits<std::uint32_t>::max()) {
    good_ = false;
    return false;
  }
  const auto payloadIndex = static_cast<std::uint32_t>(storage.size());
  if (!storage.try_emplace_back(value) ||
      !block_->commandHeaders_.try_emplace_back(MetalCommandHeader{
          .kind = kind,
          .payloadIndex = CommandPayloadIndex::fromU32(payloadIndex),
      })) {
    good_ = false;
    return false;
  }
  return true;
}

bool ArenaSourcePayloadBuilder::tryAppendClearCommand(
    const ClearDesc& clear) noexcept {
  if (!good_ || block_->commandHeaders_.remaining() == 0 ||
      block_->clearRecords_.remaining() == 0 ||
      clear.rects.size() > block_->clearRects_.remaining() ||
      block_->clearRecords_.size() > std::numeric_limits<std::uint32_t>::max() ||
      block_->clearRects_.size() > std::numeric_limits<std::uint32_t>::max() ||
      clear.rects.size() > std::numeric_limits<std::uint32_t>::max() -
                               block_->clearRects_.size()) {
    good_ = false;
    return false;
  }

  const auto payloadIndex =
      static_cast<std::uint32_t>(block_->clearRecords_.size());
  const ArenaClearRecord record{
      .colorAttachments = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
      .clearColor = clear.clearColor,
      .clearDepth = clear.clearDepth,
      .clearStencil = clear.clearStencil,
      .color = clear.color,
      .depth = clear.depth,
      .stencil = clear.stencil,
      .firstRect = static_cast<std::uint32_t>(block_->clearRects_.size()),
      .rectCount = static_cast<std::uint32_t>(clear.rects.size()),
  };
  if (!block_->clearRects_.try_append(clear.rects) ||
      !block_->clearRecords_.try_emplace_back(record) ||
      !block_->commandHeaders_.try_emplace_back(MetalCommandHeader{
          .kind = MetalCommandKind::Clear,
          .payloadIndex = CommandPayloadIndex::fromU32(payloadIndex),
      })) {
    good_ = false;
    return false;
  }
  return true;
}

bool ArenaSourcePayloadBuilder::tryAppendSurfaceCopyCommand(
    const SurfaceCopyDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::SurfaceCopy,
                             block_->surfaceCopyRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendStretchRectCommand(
    const StretchRectDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::StretchRect,
                             block_->stretchRectRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendReadbackCommand(
    const ReadbackDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::Readback,
                             block_->readbackRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendColorFillCommand(
    const ColorFillDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::ColorFill,
                             block_->colorFillRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendDepthResolveCommand(
    const DepthResolveDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::DepthResolve,
                             block_->depthResolveRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendGenerateMipmapsCommand(
    const GenerateMipmapsDesc& value) noexcept {
  return appendCommandRecord(MetalCommandKind::GenerateMipmaps,
                             block_->generateMipmapsRecords_, value);
}

bool ArenaSourcePayloadBuilder::tryAppendPresentCommand(
    PresentCommandRecord&& value) noexcept {
  if (!good_ || block_->presentRecords_.remaining() == 0 ||
      block_->commandHeaders_.remaining() == 0 ||
      block_->presentRecords_.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    good_ = false;
    return false;
  }
  const auto payloadIndex =
      static_cast<std::uint32_t>(block_->presentRecords_.size());
  if (!block_->presentRecords_.try_emplace_back(std::move(value)) ||
      !block_->commandHeaders_.try_emplace_back(MetalCommandHeader{
          .kind = MetalCommandKind::Present,
          .payloadIndex = CommandPayloadIndex::fromU32(payloadIndex),
      })) {
    good_ = false;
    return false;
  }
  return true;
}

bool TransactionalChunkSlotAssembler::reserve() noexcept {
  if (state_ != State::Empty ||
      ((!builder_ || !builder_->good()) && !directSlot_)) {
    state_ = State::Failed;
    return false;
  }
  if (directSlot_) {
    auto& slot = *directSlot_;
    if (slot.prefetchedPipelinesSealed() ||
        !slot.drawStateStorageConsistent() ||
        !slot.commandPayloadsInRange()) {
      state_ = State::Failed;
      return false;
    }
    std::size_t clearRectCount = 0;
    for (const auto& clear : slot.clearRecords) {
      if (clear.rects.size() >
          std::numeric_limits<std::size_t>::max() - clearRectCount) {
        state_ = State::Failed;
        return false;
      }
      clearRectCount += clear.rects.size();
    }
    directCheckpoint_ = {
        .commandHeaders = slot.commandHeaders.size(),
        .drawHotStates = slot.drawHotStates.size(),
        .drawShaderLayouts = slot.drawShaderLayouts.size(),
        .drawDebugSnapshots = slot.drawDebugSnapshots.size(),
        .drawPsoSubviews = slot.drawPsoSubviews.size(),
        .drawUniformFixedPayloads = slot.drawUniformFixedPayloads.size(),
        .drawUniformVertexConstants = slot.drawUniformVertexConstants.size(),
        .drawUniformVertexConstantBytes =
            slot.drawUniformVertexConstantBytes.size(),
        .drawUniformPixelConstants = slot.drawUniformPixelConstants.size(),
        .drawUniformPixelConstantBytes =
            slot.drawUniformPixelConstantBytes.size(),
        .drawUniformPayloads = slot.drawUniformPayloads.size(),
        .drawUniformPayloadLookupHeads =
            slot.drawUniformPayloadLookupHeads.size(),
        .drawUniformPayloadLookupTails =
            slot.drawUniformPayloadLookupTails.size(),
        .drawUniformPayloadLookupNext =
            slot.drawUniformPayloadLookupNext.size(),
        .drawUniformVertexConstantsLookupHeads =
            slot.drawUniformVertexConstantsLookupHeads.size(),
        .drawUniformVertexConstantsLookupTails =
            slot.drawUniformVertexConstantsLookupTails.size(),
        .drawUniformVertexConstantsLookupNext =
            slot.drawUniformVertexConstantsLookupNext.size(),
        .drawUniformPixelConstantsLookupHeads =
            slot.drawUniformPixelConstantsLookupHeads.size(),
        .drawUniformPixelConstantsLookupTails =
            slot.drawUniformPixelConstantsLookupTails.size(),
        .drawUniformPixelConstantsLookupNext =
            slot.drawUniformPixelConstantsLookupNext.size(),
        .drawParams = slot.drawParams.size(),
        .drawPayloadBytes = slot.drawPayloadArena.size(),
        .drawRunRecords = slot.drawRunRecords.size(),
        .clearRecords = slot.clearRecords.size(),
        .clearRects = clearRectCount,
        .surfaceCopyRecords = slot.surfaceCopyRecords.size(),
        .stretchRectRecords = slot.stretchRectRecords.size(),
        .readbackRecords = slot.readbackRecords.size(),
        .colorFillRecords = slot.colorFillRecords.size(),
        .depthResolveRecords = slot.depthResolveRecords.size(),
        .generateMipmapsRecords = slot.generateMipmapsRecords.size(),
        .presentRecords = slot.presentRecords.size(),
    };
    directClearRectCount_ = clearRectCount;
    directLastUniformFixedHandle_ = slot.lastUniformFixedHandle;
    directLastUniformVertexConstantsHandle_ =
        slot.lastUniformVertexConstantsHandle;
    directLastUniformPixelConstantsHandle_ =
        slot.lastUniformPixelConstantsHandle;
    directLastUniformHandle_ = slot.lastUniformHandle;
    const auto reserve = [](auto& values, std::size_t extra) {
      if (extra > std::numeric_limits<std::size_t>::max() - values.size()) {
        throw std::length_error("direct ChunkSlot capacity overflow");
      }
      values.reserve(values.size() + extra);
    };
    try {
      reserve(slot.commandHeaders, directCapacity_.commandHeaders);
      reserve(slot.drawHotStates, directCapacity_.drawHotStates);
      reserve(slot.drawShaderLayouts, directCapacity_.drawShaderLayouts);
      reserve(slot.drawDebugSnapshots, directCapacity_.drawDebugSnapshots);
      reserve(slot.drawPsoSubviews, directCapacity_.drawPsoSubviews);
      reserve(slot.drawUniformFixedPayloads,
              directCapacity_.drawUniformFixedPayloads);
      reserve(slot.drawUniformVertexConstants,
              directCapacity_.drawUniformVertexConstants);
      reserve(slot.drawUniformVertexConstantBytes,
              directCapacity_.drawUniformVertexConstantBytes);
      reserve(slot.drawUniformPixelConstants,
              directCapacity_.drawUniformPixelConstants);
      reserve(slot.drawUniformPixelConstantBytes,
              directCapacity_.drawUniformPixelConstantBytes);
      reserve(slot.drawUniformPayloads, directCapacity_.drawUniformPayloads);
      reserve(slot.drawParams, directCapacity_.drawParams);
      reserve(slot.drawPayloadArena, directCapacity_.drawPayloadBytes);
      reserve(slot.drawRunRecords, directCapacity_.drawRunRecords);
      reserve(slot.clearRecords, directCapacity_.clearRecords);
      reserve(slot.surfaceCopyRecords, directCapacity_.surfaceCopyRecords);
      reserve(slot.stretchRectRecords, directCapacity_.stretchRectRecords);
      reserve(slot.readbackRecords, directCapacity_.readbackRecords);
      reserve(slot.colorFillRecords, directCapacity_.colorFillRecords);
      reserve(slot.depthResolveRecords, directCapacity_.depthResolveRecords);
      reserve(slot.generateMipmapsRecords,
              directCapacity_.generateMipmapsRecords);
      reserve(slot.presentRecords, directCapacity_.presentRecords);
      if (directCapacity_.drawUniformPayloads != 0) {
        slot.reserveDrawUniformPayloadLookup(
            slot.drawUniformPayloads.size() +
            directCapacity_.drawUniformPayloads);
        slot.reserveDrawUniformStageLookup(
            slot.drawUniformVertexConstants,
            slot.drawUniformVertexConstantsLookupHeads,
            slot.drawUniformVertexConstantsLookupTails,
            slot.drawUniformVertexConstantsLookupNext,
            slot.drawUniformVertexConstants.size() +
                directCapacity_.drawUniformVertexConstants);
        slot.reserveDrawUniformStageLookup(
            slot.drawUniformPixelConstants,
            slot.drawUniformPixelConstantsLookupHeads,
            slot.drawUniformPixelConstantsLookupTails,
            slot.drawUniformPixelConstantsLookupNext,
            slot.drawUniformPixelConstants.size() +
                directCapacity_.drawUniformPixelConstants);
      }
    } catch (...) {
      state_ = State::Failed;
      return false;
    }
  }
  state_ = State::Reserved;
  return true;
}

bool TransactionalChunkSlotAssembler::beginBuild() noexcept {
  if (state_ == State::Reserved) {
    state_ = State::Building;
  }
  return state_ == State::Building &&
         ((builder_ && builder_->good()) || directSlot_);
}

bool TransactionalChunkSlotAssembler::bindOuter(OuterBinding binding) noexcept {
  if (state_ != State::Reserved || !binding.valid() ||
      outerBinding_.has_value()) {
    return false;
  }
  outerBinding_ = binding;
  return true;
}

bool TransactionalChunkSlotAssembler::prepare() noexcept {
  const bool destinationReady = directSlot_
      ? directSlot_->drawStateStorageConsistent() &&
            directSlot_->commandPayloadsInRange()
      : builder_ && builder_->publish();
  if ((state_ != State::Reserved && state_ != State::Building) ||
      !destinationReady) {
    state_ = State::Failed;
    return false;
  }
  state_ = State::Prepared;
  return true;
}

bool TransactionalChunkSlotAssembler::bindCommitEvidence(
    ArenaCommitEvidence&& evidence) noexcept {
  if (state_ != State::Prepared || !outerBinding_.has_value() ||
      !evidence.valid() ||
      !evidence.matchesBinding(
          outerBinding_->rawOrdinal, outerBinding_->sourceOrdinal,
          outerBinding_->seqId, outerBinding_->buildGeneration,
          outerBinding_->sourceGeneration, outerBinding_->storageGeneration,
          outerBinding_->controlIndex, outerBinding_->firstPage,
          outerBinding_->pageCount, outerBinding_->segmentIndex,
          outerBinding_->segmentCount, outerBinding_->plannedBytes) ||
      commitEvidence_.has_value()) {
    state_ = State::Failed;
    return false;
  }
  commitEvidence_.emplace(std::move(evidence));
  return true;
}

bool TransactionalChunkSlotAssembler::fail() noexcept {
  state_ = State::Failed;
  return builder_ ? builder_->reject() : false;
}

bool TransactionalChunkSlotAssembler::commitValueOnlyForTest() noexcept {
  if (state_ == State::Reserved || state_ == State::Building) {
    const bool destinationReady = directSlot_
        ? directSlot_->drawStateStorageConsistent() &&
              directSlot_->commandPayloadsInRange()
        : builder_ && builder_->publish();
    if (!destinationReady ||
        (outerBinding_.has_value() && !outerBinding_->valid())) {
      state_ = State::Failed;
      return false;
    }
    state_ = State::Committed;
    return true;
  }
  state_ = State::Failed;
  return false;
}

bool TransactionalChunkSlotAssembler::commit() noexcept {
  // Queue-owned production reaches this branch only after prepare() and a
  // queue-minted live evidence bind; no fallible operation remains here.
  if (state_ != State::Prepared || (!builder_ && !directSlot_) ||
      !commitEvidence_ ||
      !commitEvidence_->valid()) {
    state_ = State::Failed;
    return false;
  }
  state_ = State::Committed;
  return true;
}

void TransactionalChunkSlotAssembler::rollback() noexcept {
  if (state_ == State::Committed || state_ == State::RolledBack ||
      state_ == State::AbandonedFailStop) {
    return;
  }
  if (directSlot_) {
    auto& slot = *directSlot_;
    slot.commandHeaders.resize(directCheckpoint_.commandHeaders);
    slot.drawHotStates.resize(directCheckpoint_.drawHotStates);
    slot.drawShaderLayouts.resize(directCheckpoint_.drawShaderLayouts);
    slot.drawDebugSnapshots.resize(directCheckpoint_.drawDebugSnapshots);
    slot.drawPsoSubviews.resize(directCheckpoint_.drawPsoSubviews);
    slot.drawUniformFixedPayloads.resize(
        directCheckpoint_.drawUniformFixedPayloads);
    slot.drawUniformVertexConstants.resize(
        directCheckpoint_.drawUniformVertexConstants);
    slot.drawUniformVertexConstantBytes.resize(
        directCheckpoint_.drawUniformVertexConstantBytes);
    slot.drawUniformPixelConstants.resize(
        directCheckpoint_.drawUniformPixelConstants);
    slot.drawUniformPixelConstantBytes.resize(
        directCheckpoint_.drawUniformPixelConstantBytes);
    slot.drawUniformPayloads.resize(directCheckpoint_.drawUniformPayloads);
    slot.drawParams.resize(directCheckpoint_.drawParams);
    slot.drawPayloadArena.resize(directCheckpoint_.drawPayloadBytes);
    slot.drawRunRecords.resize(directCheckpoint_.drawRunRecords);
    slot.clearRecords.resize(directCheckpoint_.clearRecords);
    directClearRectCount_ = directCheckpoint_.clearRects;
    slot.surfaceCopyRecords.resize(directCheckpoint_.surfaceCopyRecords);
    slot.stretchRectRecords.resize(directCheckpoint_.stretchRectRecords);
    slot.readbackRecords.resize(directCheckpoint_.readbackRecords);
    slot.colorFillRecords.resize(directCheckpoint_.colorFillRecords);
    slot.depthResolveRecords.resize(directCheckpoint_.depthResolveRecords);
    slot.generateMipmapsRecords.resize(
        directCheckpoint_.generateMipmapsRecords);
    slot.presentRecords.resize(directCheckpoint_.presentRecords);
    slot.lastUniformFixedHandle = directLastUniformFixedHandle_;
    slot.lastUniformVertexConstantsHandle =
        directLastUniformVertexConstantsHandle_;
    slot.lastUniformPixelConstantsHandle =
        directLastUniformPixelConstantsHandle_;
    slot.lastUniformHandle = directLastUniformHandle_;
    // Rebuild the lookup arrays from the restored semantic prefix. Capacity
    // may stay warm, but no rejected transaction remains addressable.
    slot.rebuildDrawUniformPayloadLookup(
        directCheckpoint_.drawUniformPayloadLookupHeads);
    slot.rebuildDrawUniformStageLookup(
        slot.drawUniformVertexConstants,
        slot.drawUniformVertexConstantsLookupHeads,
        slot.drawUniformVertexConstantsLookupTails,
        slot.drawUniformVertexConstantsLookupNext,
        directCheckpoint_.drawUniformVertexConstantsLookupHeads);
    slot.rebuildDrawUniformStageLookup(
        slot.drawUniformPixelConstants,
        slot.drawUniformPixelConstantsLookupHeads,
        slot.drawUniformPixelConstantsLookupTails,
        slot.drawUniformPixelConstantsLookupNext,
        directCheckpoint_.drawUniformPixelConstantsLookupHeads);
  } else if (builder_) {
    builder_->rollback();
  }
  state_ = State::RolledBack;
}

void TransactionalChunkSlotAssembler::abandonFailStop() noexcept {
  state_ = State::AbandonedFailStop;
  directSlot_ = nullptr;
  builder_ = nullptr;
}

bool TransactionalChunkSlotAssembler::tryAppendUniform(
    const DrawUniformPayload& payload,
    DrawUniformHandle& uniformHandle) noexcept {
  if (!good() || uniformCount_ > std::numeric_limits<std::uint32_t>::max()) {
    return builder_->reject();
  }
  const auto index = static_cast<std::uint32_t>(uniformCount_);
  uniformHandle = detail::chunkSlotUniformHandle(index, payload.hash);
  const auto fixedHandle =
      detail::chunkSlotUniformFixedHandle(index, payload.fixedPayloadHash);
  const auto vertexHandle = detail::chunkSlotUniformStageHandle(
      index, payload.vertexConstantsHash);
  const auto pixelHandle = detail::chunkSlotUniformStageHandle(
      index, payload.pixelConstantsHash);

  auto appendStage = [&](const auto& constants,
                         DrawUniformStageConstantsSpan span,
                         bool vertex) noexcept {
    auto& cursor = vertex ? vertexConstantBytes_ : pixelConstantBytes_;
    if (cursor > std::numeric_limits<std::uint32_t>::max() ||
        span.byteSize > std::numeric_limits<std::uint32_t>::max() - cursor) {
      return false;
    }
    span.byteOffset = static_cast<std::uint32_t>(cursor);
    const auto append = [&](const void* data, std::size_t byteCount) noexcept {
      if (byteCount == 0) {
        return true;
      }
      const auto bytes = std::span<const u8>(
          static_cast<const u8*>(data), byteCount);
      std::size_t offset = 0;
      const bool appended = vertex
          ? builder_->tryAppendVertexConstantBytes(bytes, 1, offset)
          : builder_->tryAppendPixelConstantBytes(bytes, 1, offset);
      if (!appended || offset != cursor) {
        return false;
      }
      cursor += byteCount;
      return true;
    };
    const std::size_t start = cursor;
    if (!append(constants.float4.data(),
                static_cast<std::size_t>(span.floatCount) *
                    sizeof(constants.float4[0])) ||
        !append(constants.int4.data(),
                static_cast<std::size_t>(span.intCount) *
                    sizeof(constants.int4[0])) ||
        !append(constants.bools.data(),
                static_cast<std::size_t>(span.boolCount) *
                    sizeof(constants.bools[0])) ||
        cursor - start != span.byteSize) {
      return false;
    }
    return vertex
        ? builder_->tryAppendDrawUniformVertexConstants(
              DrawUniformVertexConstantsRecord{
                  .handle = vertexHandle,
                  .constants = span,
              })
        : builder_->tryAppendDrawUniformPixelConstants(
              DrawUniformPixelConstantsRecord{
                  .handle = pixelHandle,
                  .constants = span,
              });
  };

  if (!builder_->tryAppendDrawUniformFixedPayload(
          DrawUniformFixedPayloadRecord{
              .handle = fixedHandle,
              .payload = makeDrawUniformFixedPayload(payload),
          }) ||
      !appendStage(payload.vsConst,
                   makeDrawUniformVertexConstantsSpan(payload, 0), true) ||
      !appendStage(payload.psConst,
                   makeDrawUniformPixelConstantsSpan(payload, 0), false) ||
      !builder_->tryAppendDrawUniformPayload(DrawUniformPayloadRecord{
          uniformHandle,
          fixedHandle,
          vertexHandle,
          pixelHandle,
          payload,
      })) {
    return builder_->reject();
  }
  ++uniformCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendPayloadBytes(
    std::span<const u8> bytes,
    DrawPayloadRange& range,
    std::size_t runPayloadOffset) noexcept {
  if (bytes.empty()) {
    range = {};
    return true;
  }
  if (!good() || bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return builder_->reject();
  }
  std::size_t offset = 0;
  if (!builder_->tryAppendDrawPayloadBytes(
          bytes, kSourcePayloadByteAlignment, offset) ||
      offset < runPayloadOffset ||
      offset - runPayloadOffset > std::numeric_limits<std::uint32_t>::max() ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max() -
                         (offset - runPayloadOffset)) {
    return builder_->reject();
  }
  range = DrawPayloadRange{
      .offset = static_cast<std::uint32_t>(offset - runPayloadOffset),
      .size = static_cast<std::uint32_t>(bytes.size()),
  };
  drawPayloadBytes_ = offset + bytes.size();
  return true;
}


bool TransactionalChunkSlotAssembler::tryAppendDirectDraw(
    const DirectReplayDrawInput& input) noexcept {
  if (directSlot_) {
    if (!beginBuild() || !input.valid()) {
      return fail();
    }
    auto& slot = *directSlot_;
    try {
      bool extendRun = false;
      if (directRunOpen_ && directRunStateIndex_ < slot.drawHotStates.size() &&
          directRunStateIndex_ < slot.drawShaderLayouts.size()) {
        extendRun = drawStatesCompatibleForDrawRunBatch(
                        slot.drawHotStates[directRunStateIndex_], *input.hot) &&
            shaderLayoutsCompatibleForDrawRunBatch(
                slot.drawShaderLayouts[directRunStateIndex_],
                *input.shaderLayout);
      }
      const auto fits = [](std::size_t current, std::size_t checkpoint,
                           std::size_t extra, std::size_t allowance) noexcept {
        return current >= checkpoint &&
            current - checkpoint <= allowance &&
            extra <= allowance - (current - checkpoint);
      };
      std::size_t payloadBytes = 0;
      for (const auto bytes : {input.payload.userVertexData,
                               input.payload.userIndexData,
                               input.payload.bindingOverrideData,
                               input.payload.bindingSnapshotData}) {
        if (bytes.size() > std::numeric_limits<std::size_t>::max() -
                               payloadBytes) {
          return fail();
        }
        payloadBytes += bytes.size();
      }
      if (!fits(slot.commandHeaders.size(), directCheckpoint_.commandHeaders,
                extendRun ? 0u : 1u, directCapacity_.commandHeaders) ||
          !fits(slot.drawHotStates.size(), directCheckpoint_.drawHotStates,
                extendRun ? 0u : 1u, directCapacity_.drawHotStates) ||
          !fits(slot.drawShaderLayouts.size(),
                directCheckpoint_.drawShaderLayouts, extendRun ? 0u : 1u,
                directCapacity_.drawShaderLayouts) ||
          !fits(slot.drawDebugSnapshots.size(),
                directCheckpoint_.drawDebugSnapshots, extendRun ? 0u : 1u,
                directCapacity_.drawDebugSnapshots) ||
          !fits(slot.drawPsoSubviews.size(),
                directCheckpoint_.drawPsoSubviews, extendRun ? 0u : 1u,
                directCapacity_.drawPsoSubviews) ||
          !fits(slot.drawUniformFixedPayloads.size(),
                directCheckpoint_.drawUniformFixedPayloads, 1u,
                directCapacity_.drawUniformFixedPayloads) ||
          !fits(slot.drawUniformVertexConstants.size(),
                directCheckpoint_.drawUniformVertexConstants, 1u,
                directCapacity_.drawUniformVertexConstants) ||
          !fits(slot.drawUniformVertexConstantBytes.size(),
                directCheckpoint_.drawUniformVertexConstantBytes,
                sizeof(VertexShaderConstants),
                directCapacity_.drawUniformVertexConstantBytes) ||
          !fits(slot.drawUniformPixelConstants.size(),
                directCheckpoint_.drawUniformPixelConstants, 1u,
                directCapacity_.drawUniformPixelConstants) ||
          !fits(slot.drawUniformPixelConstantBytes.size(),
                directCheckpoint_.drawUniformPixelConstantBytes,
                sizeof(PixelShaderConstants),
                directCapacity_.drawUniformPixelConstantBytes) ||
          !fits(slot.drawUniformPayloads.size(),
                directCheckpoint_.drawUniformPayloads, 1u,
                directCapacity_.drawUniformPayloads) ||
          !fits(slot.drawParams.size(), directCheckpoint_.drawParams, 1u,
                directCapacity_.drawParams) ||
          !fits(slot.drawPayloadArena.size(),
                directCheckpoint_.drawPayloadBytes, payloadBytes,
                directCapacity_.drawPayloadBytes) ||
          !fits(slot.drawRunRecords.size(), directCheckpoint_.drawRunRecords,
                extendRun ? 0u : 1u, directCapacity_.drawRunRecords)) {
        return fail();
      }
      const auto finalBaseBytes = sizeof(DrawParam) +
          sizeof(DrawUniformPayload) +
          (extendRun ? 0u
                     : sizeof(FlatDrawStateRecord) +
                           sizeof(DrawShaderLayoutContext) +
                           sizeof(DrawDebugSnapshot));
      const auto drawPayloadBytesBefore = slot.drawPayloadArena.size();
      std::optional<dxmt9::core::CopyMaterializationEvent> finalAppendEvent;
      if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
              dxmt9::core::CopyMaterializationOwner::Unix)) {
        finalAppendEvent.emplace(
            ledger,
            dxmt9::core::CopyMaterializationClass::QueueFinalSlotAppend,
            finalBaseBytes);
      }

      std::uint32_t stateIndex = 0;
      DrawPsoSubview psoSubview{};
      DrawRunInvariant invariant{};
      const std::size_t runPayloadOffset = extendRun
          ? directRunPayloadOffset_
          : slot.drawPayloadArena.size();
      if (!extendRun) {
        if (slot.drawHotStates.size() >
            std::numeric_limits<std::uint32_t>::max()) {
          return fail();
        }
        stateIndex = static_cast<std::uint32_t>(slot.drawHotStates.size());
        psoSubview = ChunkSlot::makeDrawPsoSubview(
            *input.hot, *input.shaderLayout);
        invariant = ChunkSlot::makeDrawRunInvariant(*input.hot);
        slot.drawHotStates.push_back(*input.hot);
        slot.drawShaderLayouts.push_back(*input.shaderLayout);
        slot.drawDebugSnapshots.push_back(input.debug);
      }

      const auto uniformHandle =
          slot.appendDrawUniformPayload(*input.uniforms);
      if (!uniformHandle.valid() ||
          slot.drawParams.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail();
      }
      DrawParam draw = input.draw;
      draw.uniformHandle = uniformHandle;
      auto appendPayload = [&](std::span<const u8> bytes,
                               DrawPayloadRange& range) noexcept {
        if (bytes.empty()) {
          range = {};
          return true;
        }
        const auto offset = slot.drawPayloadArena.size();
        if (offset < runPayloadOffset ||
            offset - runPayloadOffset >
                std::numeric_limits<std::uint32_t>::max() ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max() -
                               (offset - runPayloadOffset)) {
          return false;
        }
        slot.drawPayloadArena.insert(slot.drawPayloadArena.end(),
                                     bytes.begin(), bytes.end());
        range = {
            .offset = static_cast<std::uint32_t>(offset - runPayloadOffset),
            .size = static_cast<std::uint32_t>(bytes.size()),
        };
        return true;
      };
      if (!appendPayload(input.payload.userVertexData, draw.userVertexRange) ||
          !appendPayload(input.payload.userIndexData, draw.userIndexRange) ||
          !appendPayload(input.payload.bindingOverrideData,
                         draw.bindingOverrideRange) ||
          !appendPayload(input.payload.bindingSnapshotData,
                         draw.bindingSnapshotRange)) {
        return fail();
      }
      const auto firstParam = static_cast<std::uint32_t>(
          slot.drawParams.size());
      slot.drawParams.push_back(draw);
      if (finalAppendEvent) {
        finalAppendEvent->setBytes(
            finalBaseBytes +
            (slot.drawPayloadArena.size() - drawPayloadBytesBefore));
      }

      if (extendRun) {
        if (directRunRecordIndex_ >= slot.drawRunRecords.size()) {
          return fail();
        }
        auto& run = slot.drawRunRecords[directRunRecordIndex_];
        if (run.paramCount == std::numeric_limits<std::uint32_t>::max() ||
            slot.drawPayloadArena.size() < runPayloadOffset ||
            slot.drawPayloadArena.size() - runPayloadOffset >
                std::numeric_limits<std::uint32_t>::max()) {
          return fail();
        }
        ++run.paramCount;
        run.payloadSize = static_cast<std::uint32_t>(
            slot.drawPayloadArena.size() - runPayloadOffset);
        return true;
      }

      if (slot.drawRunRecords.size() >
              std::numeric_limits<std::uint32_t>::max() ||
          slot.drawPayloadArena.size() < runPayloadOffset ||
          slot.drawPayloadArena.size() - runPayloadOffset >
              std::numeric_limits<std::uint32_t>::max()) {
        return fail();
      }
      const auto runIndex = static_cast<std::uint32_t>(
          slot.drawRunRecords.size());
      slot.drawPsoSubviews.push_back(psoSubview);
      slot.drawRunRecords.push_back(DrawRunCommandRecord{
          .stateIndex = stateIndex,
          .firstParam = firstParam,
          .paramCount = 1u,
          .payloadOffset = static_cast<std::uint32_t>(runPayloadOffset),
          .payloadSize = static_cast<std::uint32_t>(
              slot.drawPayloadArena.size() - runPayloadOffset),
          .uniformHandle = uniformHandle,
          .invariant = invariant,
      });
      slot.commandHeaders.push_back(MetalCommandHeader{
          .kind = MetalCommandKind::DrawRun,
          .payloadIndex = CommandPayloadIndex::fromU32(runIndex),
      });
      directRunStateIndex_ = stateIndex;
      directRunRecordIndex_ = runIndex;
      directRunPayloadOffset_ = runPayloadOffset;
      directRunOpen_ = true;
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !input.valid() ||
      stateCount_ > std::numeric_limits<std::uint32_t>::max() ||
      uniformCount_ > std::numeric_limits<std::uint32_t>::max() ||
      paramCount_ > std::numeric_limits<std::uint32_t>::max() ||
      drawRunCount_ > std::numeric_limits<std::uint32_t>::max() ||
      drawPayloadBytes_ > std::numeric_limits<std::uint32_t>::max()) {
    return fail();
  }

  bool extendRun = false;
  if (directRunOpen_) {
    const auto* baseHot = builder_->drawHotState(directRunStateIndex_);
    const auto* baseLayout =
        builder_->drawShaderLayout(directRunStateIndex_);
    extendRun = baseHot && baseLayout &&
        drawStatesCompatibleForDrawRunBatch(*baseHot, *input.hot) &&
        shaderLayoutsCompatibleForDrawRunBatch(*baseLayout,
                                               *input.shaderLayout);
  }
  const auto finalBaseBytes = sizeof(DrawParam) + sizeof(DrawUniformPayload) +
      (extendRun ? 0u : sizeof(FlatDrawStateRecord) +
          sizeof(DrawShaderLayoutContext) + sizeof(DrawDebugSnapshot));
  const auto drawPayloadBytesBefore = drawPayloadBytes_;
  std::optional<dxmt9::core::CopyMaterializationEvent> finalAppendEvent;
  if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
          dxmt9::core::CopyMaterializationOwner::Unix)) {
    finalAppendEvent.emplace(
        ledger,
        dxmt9::core::CopyMaterializationClass::QueueFinalSlotAppend,
        finalBaseBytes);
  }

  std::uint32_t stateIndex = 0;
  DrawPsoSubview psoSubview{};
  DrawRunInvariant invariant{};
  const std::size_t runPayloadOffset = extendRun
      ? directRunPayloadOffset_
      : drawPayloadBytes_;
  if (!extendRun) {
    stateIndex = static_cast<std::uint32_t>(stateCount_);
    psoSubview = ChunkSlot::makeDrawPsoSubview(
        *input.hot, *input.shaderLayout);
    invariant = ChunkSlot::makeDrawRunInvariant(*input.hot);
    if (!builder_->tryAppendDrawHotState(*input.hot) ||
        !builder_->tryAppendDrawShaderLayout(*input.shaderLayout) ||
        !builder_->tryAppendDrawDebugSnapshot(input.debug)) {
      return fail();
    }
    ++stateCount_;
  }

  DrawUniformHandle uniformHandle{};
  if (!tryAppendUniform(*input.uniforms, uniformHandle)) {
    return fail();
  }
  DrawParam draw = input.draw;
  draw.uniformHandle = uniformHandle;
  if (!tryAppendPayloadBytes(input.payload.userVertexData,
                             draw.userVertexRange, runPayloadOffset) ||
      !tryAppendPayloadBytes(input.payload.userIndexData,
                             draw.userIndexRange, runPayloadOffset) ||
      !tryAppendPayloadBytes(input.payload.bindingOverrideData,
                             draw.bindingOverrideRange, runPayloadOffset) ||
      !tryAppendPayloadBytes(input.payload.bindingSnapshotData,
                             draw.bindingSnapshotRange, runPayloadOffset) ||
      !builder_->tryAppendDrawParam(draw)) {
    return fail();
  }
  ++paramCount_;
  if (finalAppendEvent) {
    finalAppendEvent->setBytes(
        finalBaseBytes + (drawPayloadBytes_ - drawPayloadBytesBefore));
  }

  if (extendRun) {
    auto* run = builder_->drawRun(directRunRecordIndex_);
    if (!run || run->paramCount == std::numeric_limits<std::uint32_t>::max() ||
        drawPayloadBytes_ < runPayloadOffset ||
        drawPayloadBytes_ - runPayloadOffset >
            std::numeric_limits<std::uint32_t>::max()) {
      return fail();
    }
    ++run->paramCount;
    run->payloadSize = static_cast<std::uint32_t>(
        drawPayloadBytes_ - runPayloadOffset);
    return true;
  }

  if (drawPayloadBytes_ < runPayloadOffset ||
      drawPayloadBytes_ - runPayloadOffset >
          std::numeric_limits<std::uint32_t>::max() ||
      !builder_->tryAppendDrawPsoSubview(psoSubview) ||
      !builder_->tryAppendDrawRun(DrawRunCommandRecord{
          .stateIndex = stateIndex,
          .firstParam = static_cast<std::uint32_t>(paramCount_ - 1u),
          .paramCount = 1u,
          .payloadOffset = static_cast<std::uint32_t>(runPayloadOffset),
          .payloadSize = static_cast<std::uint32_t>(
              drawPayloadBytes_ - runPayloadOffset),
          .uniformHandle = uniformHandle,
          .invariant = invariant,
      }) ||
      !builder_->tryAppendCommand(
          MetalCommandKind::DrawRun,
          static_cast<std::uint32_t>(drawRunCount_))) {
    return fail();
  }
  directRunStateIndex_ = stateIndex;
  directRunRecordIndex_ = drawRunCount_;
  directRunPayloadOffset_ = runPayloadOffset;
  directRunOpen_ = true;
  ++drawRunCount_;
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendClear(
    const ClearDesc& value) noexcept {
  if (!directSlot_) {
    directRunOpen_ = false;
    if (!beginBuild() || !builder_->tryAppendClearCommand(value)) {
      return builder_->reject();
    }
    ++commandCount_;
    return true;
  }
  // A non-empty direct clear must transfer an owned rect vector. Copying a
  // ClearDesc here would allocate after reserve(); the direct queue makes the
  // copy before beginning the transaction and calls the rvalue overload.
  if (directSlot_ && !value.rects.empty()) {
    return fail();
  }
  ClearDesc owned = value;
  return tryAppendClear(std::move(owned));
}

bool TransactionalChunkSlotAssembler::tryAppendClear(
    ClearDesc&& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    const auto rectCount = value.rects.size();
    const auto rectsUsed = directClearRectCount_ >=
                                   directCheckpoint_.clearRects
                               ? directClearRectCount_ -
                                     directCheckpoint_.clearRects
                               : std::numeric_limits<std::size_t>::max();
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->clearRecords.size(),
                                 directCheckpoint_.clearRecords,
                                 directCapacity_.clearRecords) ||
        rectsUsed > directCapacity_.clearRects ||
        rectCount > directCapacity_.clearRects - rectsUsed ||
        rectCount >
            std::numeric_limits<std::size_t>::max() - directClearRectCount_) {
      return fail();
    }
    try {
      directSlot_->appendClear(std::move(value));
      directClearRectCount_ += rectCount;
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendClearCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendSurfaceCopy(
    const SurfaceCopyDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->surfaceCopyRecords.size(),
                                 directCheckpoint_.surfaceCopyRecords,
                                 directCapacity_.surfaceCopyRecords)) {
      return fail();
    }
    try {
      directSlot_->appendSurfaceCopy(value);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendSurfaceCopyCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendStretchRect(
    const StretchRectDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->stretchRectRecords.size(),
                                 directCheckpoint_.stretchRectRecords,
                                 directCapacity_.stretchRectRecords)) {
      return fail();
    }
    try {
      directSlot_->appendStretchRect(value);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendStretchRectCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendReadback(
    const ReadbackDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) return fail();
  if (!beginBuild() || !builder_->tryAppendReadbackCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendColorFill(
    const ColorFillDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->colorFillRecords.size(),
                                 directCheckpoint_.colorFillRecords,
                                 directCapacity_.colorFillRecords)) {
      return fail();
    }
    try {
      directSlot_->appendColorFill(value);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendColorFillCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendDepthResolve(
    const DepthResolveDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->depthResolveRecords.size(),
                                 directCheckpoint_.depthResolveRecords,
                                 directCapacity_.depthResolveRecords)) {
      return fail();
    }
    try {
      directSlot_->appendDepthResolve(value);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendDepthResolveCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendGenerateMipmaps(
    const GenerateMipmapsDesc& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(
            directSlot_->generateMipmapsRecords.size(),
            directCheckpoint_.generateMipmapsRecords,
            directCapacity_.generateMipmapsRecords)) {
      return fail();
    }
    try {
      directSlot_->appendGenerateMipmaps(value);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendGenerateMipmapsCommand(value)) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool TransactionalChunkSlotAssembler::tryAppendPresent(
    PresentCommandRecord&& value) noexcept {
  directRunOpen_ = false;
  if (directSlot_) {
    if (!beginBuild() ||
        !directCapacityAvailable(directSlot_->commandHeaders.size(),
                                 directCheckpoint_.commandHeaders,
                                 directCapacity_.commandHeaders) ||
        !directCapacityAvailable(directSlot_->presentRecords.size(),
                                 directCheckpoint_.presentRecords,
                                 directCapacity_.presentRecords)) {
      return fail();
    }
    try {
      directSlot_->appendPresent(value.present, value.presentSource);
      ++commandCount_;
      return true;
    } catch (...) {
      return fail();
    }
  }
  if (!beginBuild() || !builder_->tryAppendPresentCommand(std::move(value))) {
    return builder_->reject();
  }
  ++commandCount_;
  return true;
}

bool ArenaSourcePayloadChain::initialize(
    std::span<const ArenaSourcePayloadBlock* const> segments) noexcept {
  if (readable_ || segments.empty() ||
      segments.size() > kMaxArenaSourcePayloadSegments) {
    return false;
  }
  std::array<const ArenaSourcePayloadBlock*,
             kMaxArenaSourcePayloadSegments> nextSegments{};
  std::array<std::size_t, kMaxArenaSourcePayloadSegments + 1> nextOffsets{};
  std::size_t commandCount = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto* segment = segments[i];
    if (!segment || !segment->readable() ||
        segment->commandHeaders_.size() >
            std::numeric_limits<std::size_t>::max() - commandCount) {
      return false;
    }
    nextSegments[i] = segment;
    nextOffsets[i] = commandCount;
    commandCount += segment->commandHeaders_.size();
  }
  nextOffsets[segments.size()] = commandCount;
  segments_ = nextSegments;
  commandOffsets_ = nextOffsets;
  segmentCount_ = segments.size();
  readable_ = true;
  return true;
}

void ArenaSourcePayloadChain::clear() noexcept {
  segments_ = {};
  commandOffsets_ = {};
  segmentCount_ = 0;
  readable_ = false;
}

bool ArenaSourcePayloadChain::locateCommand(
    std::size_t logicalCommandIndex,
    std::size_t& segmentIndex,
    std::size_t& localCommandIndex) const noexcept {
  if (!readable_ || logicalCommandIndex >= commandCount()) {
    return false;
  }
  for (std::size_t i = 0; i < segmentCount_; ++i) {
    if (logicalCommandIndex < commandOffsets_[i + 1]) {
      segmentIndex = i;
      localCommandIndex = logicalCommandIndex - commandOffsets_[i];
      return true;
    }
  }
  return false;
}

std::size_t SourcePayloadView::commandCount() const noexcept {
  if (legacy_) {
    return legacy_->commandCount();
  }
  if (arena_) {
    return arena_->commandHeaders_.size();
  }
  return arenaChain_ ? arenaChain_->commandCount() : 0;
}

std::size_t SourcePayloadView::presentRecordCount() const noexcept {
  if (legacy_) {
    return legacy_->presentRecords.size();
  }
  if (arena_) {
    return arena_->presentRecords_.size();
  }
  std::size_t count = 0;
  if (arenaChain_) {
    for (std::size_t i = 0; i < arenaChain_->segmentCount(); ++i) {
      count += arenaChain_->segment(i)->presentRecords_.size();
    }
  }
  return count;
}

bool SourcePayloadView::drawOnlyCommandStream() const noexcept {
  if (legacy_) {
    return legacy_->drawOnlyCommandStream();
  }
  if (!isArena() || commandCount() == 0) {
    return false;
  }
  for (std::size_t i = 0; i < commandCount(); ++i) {
    if (commandAt(i).kind() != MetalCommandKind::DrawRun) {
      return false;
    }
  }
  return true;
}

bool SourcePayloadView::locateCommand(
    std::size_t logicalCommandIndex,
    std::size_t& segmentIndex,
    std::size_t& localCommandIndex) const noexcept {
  if (legacy_) {
    if (logicalCommandIndex >= legacy_->commandCount()) {
      return false;
    }
    segmentIndex = 0;
    localCommandIndex = logicalCommandIndex;
    return true;
  }
  if (arena_) {
    if (logicalCommandIndex >= arena_->commandHeaders_.size()) {
      return false;
    }
    segmentIndex = 0;
    localCommandIndex = logicalCommandIndex;
    return true;
  }
  return arenaChain_ && arenaChain_->locateCommand(
                            logicalCommandIndex, segmentIndex,
                            localCommandIndex);
}

SourceCommandView SourcePayloadView::commandAt(std::size_t index) const noexcept {
  if (legacy_) {
    if (index >= legacy_->commandHeaders.size()) {
      return {};
    }
    SourceCommandView result{
        .command = legacy_->commandAt(index),
        .payloadIndex = legacy_->commandHeaders[index].payloadIndex.value,
        .localCommandIndex = index,
    };
    if (result.command.clear) {
      result.clear = makeClearView(*result.command.clear);
    }
    return result;
  }
  if (arenaChain_) {
    std::size_t segmentIndex = 0;
    std::size_t localCommandIndex = 0;
    if (!arenaChain_->locateCommand(index, segmentIndex,
                                    localCommandIndex)) {
      return {};
    }
    SourceCommandView result =
        SourcePayloadView(*arenaChain_->segment(segmentIndex))
            .commandAt(localCommandIndex);
    result.segmentIndex = segmentIndex;
    result.localCommandIndex = localCommandIndex;
    return result;
  }
  if (!arena_ || index >= arena_->commandHeaders_.size()) {
    return {};
  }

  const auto& header = arena_->commandHeaders_[index];
  SourceCommandView result{
      .payloadIndex = header.payloadIndex.value,
      .localCommandIndex = index,
  };
  result.command.kind = header.kind;
  const auto payloadIndex = header.payloadIndex.value;
  switch (header.kind) {
  case MetalCommandKind::DrawRun: {
    if (payloadIndex >= arena_->drawRunRecords_.size()) {
      break;
    }
    const auto& record = arena_->drawRunRecords_[payloadIndex];
    result.command.drawRunRecord = &record;
    result.command.drawRunInvariant = &record.invariant;
    if (payloadIndex < arena_->drawPsoSubviews_.size()) {
      result.command.drawPsoSubview = &arena_->drawPsoSubviews_[payloadIndex];
    }
    if (record.stateIndex < arena_->drawHotStates_.size() &&
        record.stateIndex < arena_->drawShaderLayouts_.size() &&
        record.stateIndex < arena_->drawDebugSnapshots_.size()) {
      result.command.drawState.hot = &arena_->drawHotStates_[record.stateIndex];
      result.command.drawState.shaderLayout =
          &arena_->drawShaderLayouts_[record.stateIndex];
      result.command.drawState.debug =
          &arena_->drawDebugSnapshots_[record.stateIndex];
    }
    result.command.drawUniformFixedPayloadRecords =
        arena_->drawUniformFixedPayloads_.span();
    result.command.drawUniformVertexConstantsRecords =
        arena_->drawUniformVertexConstants_.span();
    result.command.drawUniformVertexConstantBytes =
        asU8(arena_->drawUniformVertexConstantBytes_.span());
    result.command.drawUniformPixelConstantsRecords =
        arena_->drawUniformPixelConstants_.span();
    result.command.drawUniformPixelConstantBytes =
        asU8(arena_->drawUniformPixelConstantBytes_.span());
    result.command.drawUniformPayloadRecords =
        arena_->drawUniformPayloads_.span();
    if (record.firstParam <= arena_->drawParams_.size() &&
        record.paramCount <= arena_->drawParams_.size() - record.firstParam) {
      result.command.drawParams = arena_->drawParams_.span().subspan(
          record.firstParam, record.paramCount);
      result.command.drawItems = result.command.drawParams;
    }
    const auto payload = asU8(arena_->drawPayloadBytes_.span());
    if (record.payloadOffset <= payload.size() &&
        record.payloadSize <= payload.size() - record.payloadOffset) {
      result.command.drawPayloadBytes = payload.subspan(
          record.payloadOffset, record.payloadSize);
    }
    break;
  }
  case MetalCommandKind::Clear:
    if (payloadIndex < arena_->clearRecords_.size()) {
      result.clear = makeClearView(arena_->clearRecords_[payloadIndex],
                                   arena_->clearRects_.span());
    }
    break;
  case MetalCommandKind::SurfaceCopy:
    if (payloadIndex < arena_->surfaceCopyRecords_.size()) {
      result.command.surfaceCopy = &arena_->surfaceCopyRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::StretchRect:
    if (payloadIndex < arena_->stretchRectRecords_.size()) {
      result.command.stretchRect = &arena_->stretchRectRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::Readback:
    if (payloadIndex < arena_->readbackRecords_.size()) {
      result.command.readback = &arena_->readbackRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::ColorFill:
    if (payloadIndex < arena_->colorFillRecords_.size()) {
      result.command.colorFill = &arena_->colorFillRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::DepthResolve:
    if (payloadIndex < arena_->depthResolveRecords_.size()) {
      result.command.depthResolve = &arena_->depthResolveRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::GenerateMipmaps:
    if (payloadIndex < arena_->generateMipmapsRecords_.size()) {
      result.command.generateMipmaps =
          &arena_->generateMipmapsRecords_[payloadIndex];
    }
    break;
  case MetalCommandKind::Present:
    if (payloadIndex < arena_->presentRecords_.size()) {
      result.command.present = &arena_->presentRecords_[payloadIndex];
    }
    break;
  }
  return result;
}

std::span<const DrawUniformPayloadRecord>
SourcePayloadView::drawUniformPayloads() const noexcept {
  if (legacy_) {
    return legacy_->drawUniformPayloads;
  }
  return arena_ ? arena_->drawUniformPayloads_.span()
                : std::span<const DrawUniformPayloadRecord>{};
}

std::span<const u8>
SourcePayloadView::drawUniformVertexConstantBytes() const noexcept {
  if (legacy_) {
    return legacy_->drawUniformVertexConstantBytes;
  }
  return arena_ ? asU8(arena_->drawUniformVertexConstantBytes_.span())
                : std::span<const u8>{};
}

std::span<const u8>
SourcePayloadView::drawUniformPixelConstantBytes() const noexcept {
  if (legacy_) {
    return legacy_->drawUniformPixelConstantBytes;
  }
  return arena_ ? asU8(arena_->drawUniformPixelConstantBytes_.span())
                : std::span<const u8>{};
}

std::span<const DrawParam> SourcePayloadView::drawParams() const noexcept {
  if (legacy_) {
    return legacy_->drawParams;
  }
  return arena_ ? arena_->drawParams_.span() : std::span<const DrawParam>{};
}

std::span<const u8> SourcePayloadView::drawPayloadBytes() const noexcept {
  if (legacy_) {
    return legacy_->drawPayloadArena;
  }
  return arena_ ? asU8(arena_->drawPayloadBytes_.span())
                : std::span<const u8>{};
}

}  // namespace dxmt9::core
