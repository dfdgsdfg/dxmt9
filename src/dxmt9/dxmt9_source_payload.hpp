#pragma once

#include "dxmt9_backend_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9 {
class CommandQueue;
}

namespace dxmt9::core {

// Native-only evidence minted by CommandQueue after the payload has been
// prepared and every resource retain/mark obligation has been discharged.
// The constructor is private so an assembler cannot be made to look live by a
// caller that merely knows the value fields; only the queue can mint evidence
// tied to its active ArenaBuildContext.
class ArenaLiveTransactionToken final {
  friend class dxmt9::CommandQueue;
  friend class ArenaCommitEvidence;

  explicit ArenaLiveTransactionToken(const void* owner) noexcept
      : owner_(owner) {}

 public:
  ArenaLiveTransactionToken(const ArenaLiveTransactionToken&) = delete;
  ArenaLiveTransactionToken& operator=(const ArenaLiveTransactionToken&) = delete;
  ArenaLiveTransactionToken(ArenaLiveTransactionToken&& other) noexcept
      : owner_(other.owner_) {
    other.owner_ = nullptr;
  }
  ArenaLiveTransactionToken& operator=(ArenaLiveTransactionToken&& other) noexcept {
    if (this != &other) {
      owner_ = other.owner_;
      other.owner_ = nullptr;
    }
    return *this;
  }

 private:
  const void* owner_ = nullptr;
};

class ArenaCommitEvidence final {
  friend class dxmt9::CommandQueue;

  ArenaCommitEvidence(
      ArenaLiveTransactionToken&& liveTransaction, std::uint64_t rawOrdinal,
      std::uint64_t sourceOrdinal, std::uint64_t seqId,
      std::uint64_t buildGeneration, std::uint64_t sourceGeneration,
      std::uint64_t storageGeneration, std::uint32_t controlIndex,
      std::uint32_t firstPage, std::uint32_t pageCount,
      std::uint32_t segmentIndex, std::uint32_t segmentCount,
      std::size_t plannedBytes, std::uint64_t resourceCount,
      std::uint64_t resourceDigest) noexcept
      : liveTransaction_(liveTransaction.owner_),
        rawOrdinal_(rawOrdinal),
        sourceOrdinal_(sourceOrdinal),
        seqId_(seqId),
        buildGeneration_(buildGeneration),
        sourceGeneration_(sourceGeneration),
        storageGeneration_(storageGeneration),
        controlIndex_(controlIndex),
        firstPage_(firstPage),
        pageCount_(pageCount),
        segmentIndex_(segmentIndex),
        segmentCount_(segmentCount),
        plannedBytes_(plannedBytes),
        resourceCount_(resourceCount),
        resourceDigest_(resourceDigest) {
    liveTransaction.owner_ = nullptr;
  }

 public:
  ArenaCommitEvidence(const ArenaCommitEvidence&) = delete;
  ArenaCommitEvidence& operator=(const ArenaCommitEvidence&) = delete;
  ArenaCommitEvidence(ArenaCommitEvidence&& other) noexcept
      : liveTransaction_(other.liveTransaction_),
        rawOrdinal_(other.rawOrdinal_),
        sourceOrdinal_(other.sourceOrdinal_),
        seqId_(other.seqId_),
        buildGeneration_(other.buildGeneration_),
        sourceGeneration_(other.sourceGeneration_),
        storageGeneration_(other.storageGeneration_),
        controlIndex_(other.controlIndex_),
        firstPage_(other.firstPage_),
        pageCount_(other.pageCount_),
        segmentIndex_(other.segmentIndex_),
        segmentCount_(other.segmentCount_),
        plannedBytes_(other.plannedBytes_),
        resourceCount_(other.resourceCount_),
        resourceDigest_(other.resourceDigest_) {
    other.invalidate();
  }
  ArenaCommitEvidence& operator=(ArenaCommitEvidence&& other) noexcept {
    if (this != &other) {
      liveTransaction_ = other.liveTransaction_;
      rawOrdinal_ = other.rawOrdinal_;
      sourceOrdinal_ = other.sourceOrdinal_;
      seqId_ = other.seqId_;
      buildGeneration_ = other.buildGeneration_;
      sourceGeneration_ = other.sourceGeneration_;
      storageGeneration_ = other.storageGeneration_;
      controlIndex_ = other.controlIndex_;
      firstPage_ = other.firstPage_;
      pageCount_ = other.pageCount_;
      segmentIndex_ = other.segmentIndex_;
      segmentCount_ = other.segmentCount_;
      plannedBytes_ = other.plannedBytes_;
      resourceCount_ = other.resourceCount_;
      resourceDigest_ = other.resourceDigest_;
      other.invalidate();
    }
    return *this;
  }

  bool valid() const noexcept {
    return liveTransaction_ != nullptr && rawOrdinal_ != 0 &&
           sourceOrdinal_ != 0 && seqId_ != 0 && buildGeneration_ != 0 &&
           sourceGeneration_ != 0 && storageGeneration_ != 0 &&
           controlIndex_ != std::numeric_limits<std::uint32_t>::max() &&
           firstPage_ != std::numeric_limits<std::uint32_t>::max() &&
           pageCount_ != 0 && segmentCount_ != 0 &&
           segmentIndex_ < segmentCount_ && plannedBytes_ != 0;
  }

  bool matches(const void* liveTransaction, std::uint64_t rawOrdinal,
               std::uint64_t sourceOrdinal, std::uint64_t seqId,
               std::uint64_t buildGeneration, std::uint64_t sourceGeneration,
               std::uint64_t storageGeneration, std::uint32_t controlIndex,
               std::uint32_t firstPage, std::uint32_t pageCount,
               std::uint32_t segmentIndex, std::uint32_t segmentCount,
               std::size_t plannedBytes, std::uint64_t resourceCount,
               std::uint64_t resourceDigest) const noexcept {
    return valid() && liveTransaction_ == liveTransaction &&
           rawOrdinal_ == rawOrdinal && sourceOrdinal_ == sourceOrdinal &&
           seqId_ == seqId && buildGeneration_ == buildGeneration &&
           sourceGeneration_ == sourceGeneration &&
           storageGeneration_ == storageGeneration &&
           controlIndex_ == controlIndex && firstPage_ == firstPage &&
           pageCount_ == pageCount && segmentIndex_ == segmentIndex &&
           segmentCount_ == segmentCount && plannedBytes_ == plannedBytes &&
           resourceCount_ == resourceCount && resourceDigest_ == resourceDigest;
  }

  bool matchesBinding(std::uint64_t rawOrdinal, std::uint64_t sourceOrdinal,
                      std::uint64_t seqId, std::uint64_t buildGeneration,
                      std::uint64_t sourceGeneration,
                      std::uint64_t storageGeneration,
                      std::uint32_t controlIndex, std::uint32_t firstPage,
                      std::uint32_t pageCount, std::uint32_t segmentIndex,
                      std::uint32_t segmentCount,
                      std::size_t plannedBytes) const noexcept {
    return valid() && rawOrdinal_ == rawOrdinal &&
           sourceOrdinal_ == sourceOrdinal && seqId_ == seqId &&
           buildGeneration_ == buildGeneration &&
           sourceGeneration_ == sourceGeneration &&
           storageGeneration_ == storageGeneration &&
           controlIndex_ == controlIndex && firstPage_ == firstPage &&
           pageCount_ == pageCount && segmentIndex_ == segmentIndex &&
           segmentCount_ == segmentCount && plannedBytes_ == plannedBytes;
  }

  std::uint64_t resourceCount() const noexcept { return resourceCount_; }
  std::uint64_t resourceDigest() const noexcept { return resourceDigest_; }

 private:
  void invalidate() noexcept {
    liveTransaction_ = nullptr;
    rawOrdinal_ = sourceOrdinal_ = seqId_ = buildGeneration_ = 0;
    sourceGeneration_ = storageGeneration_ = 0;
    controlIndex_ = firstPage_ = std::numeric_limits<std::uint32_t>::max();
    pageCount_ = segmentCount_ = 0;
    segmentIndex_ = 0;
    plannedBytes_ = resourceCount_ = resourceDigest_ = 0;
  }

  const void* liveTransaction_ = nullptr;
  std::uint64_t rawOrdinal_ = 0;
  std::uint64_t sourceOrdinal_ = 0;
  std::uint64_t seqId_ = 0;
  std::uint64_t buildGeneration_ = 0;
  std::uint64_t sourceGeneration_ = 0;
  std::uint64_t storageGeneration_ = 0;
  std::uint32_t controlIndex_ = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t firstPage_ = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pageCount_ = 0;
  std::uint32_t segmentIndex_ = 0;
  std::uint32_t segmentCount_ = 0;
  std::size_t plannedBytes_ = 0;
  std::uint64_t resourceCount_ = 0;
  std::uint64_t resourceDigest_ = 0;
};

// Fixed-capacity storage over one caller-owned final extent. This container
// never allocates, relocates, copies, or falls back to another allocator.
template <typename T>
class ArenaSoA {
 public:
  ArenaSoA() = default;
  ~ArenaSoA() { destroyConstructed(); }

  ArenaSoA(const ArenaSoA&) = delete;
  ArenaSoA& operator=(const ArenaSoA&) = delete;
  ArenaSoA(ArenaSoA&&) = delete;
  ArenaSoA& operator=(ArenaSoA&&) = delete;

  bool bind(std::span<std::byte> memory, std::size_t capacity) noexcept {
    if (bound_ || size_ != 0 ||
        capacity > std::numeric_limits<std::size_t>::max() / sizeof(T) ||
        memory.size() != capacity * sizeof(T)) {
      return false;
    }
    if (capacity != 0 &&
        (memory.data() == nullptr ||
         reinterpret_cast<std::uintptr_t>(memory.data()) % alignof(T) != 0)) {
      return false;
    }
    resource_.emplace(memory.data(), memory.size(),
                      std::pmr::null_memory_resource());
    if (capacity != 0) {
      try {
        std::pmr::polymorphic_allocator<T> allocator(&*resource_);
        data_ = allocator.allocate(capacity);
      } catch (const std::bad_alloc&) {
        resource_.reset();
        return false;
      }
      if (static_cast<void*>(data_) != static_cast<void*>(memory.data())) {
        data_ = nullptr;
        resource_.reset();
        return false;
      }
    }
    capacity_ = capacity;
    bound_ = true;
    return true;
  }

  bool bound() const noexcept { return bound_; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t remaining() const noexcept { return capacity_ - size_; }
  bool empty() const noexcept { return size_ == 0; }
  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::span<T> span() noexcept { return {data_, size_}; }
  std::span<const T> span() const noexcept { return {data_, size_}; }
  T& operator[](std::size_t index) noexcept { return data_[index]; }
  const T& operator[](std::size_t index) const noexcept { return data_[index]; }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  bool try_emplace_back(Args&&... args) noexcept {
    if (!bound_ || sealed_ || size_ == capacity_) {
      return false;
    }
    std::construct_at(data_ + size_, std::forward<Args>(args)...);
    ++size_;
    return true;
  }

  bool try_append(std::span<const T> values) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    if (!bound_ || sealed_ || values.size() > capacity_ - size_) {
      return false;
    }
    for (const T& value : values) {
      std::construct_at(data_ + size_, value);
      ++size_;
    }
    return true;
  }

  // Some final owners (notably DrawShaderLayoutContext) are copyable but do
  // not advertise a nothrow copy constructor.  Direct replay must copy the
  // cache-owned value into final storage synchronously; catch the only
  // fallible construction seam before advancing the constructed prefix.
  bool try_copy_back(const T& value) noexcept
    requires std::is_copy_constructible_v<T>
  {
    if (!bound_ || sealed_ || size_ == capacity_) {
      return false;
    }
    try {
      std::construct_at(data_ + size_, value);
    } catch (...) {
      return false;
    }
    ++size_;
    return true;
  }

  void destroyConstructed() noexcept {
    while (size_ != 0) {
      --size_;
      std::destroy_at(data_ + size_);
    }
  }

  void seal() noexcept { sealed_ = true; }

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  bool bound_ = false;
  bool sealed_ = false;
  std::optional<std::pmr::monotonic_buffer_resource> resource_{};
};

inline constexpr std::size_t kSourcePayloadByteAlignment =
    alignof(std::max_align_t);

class ArenaByteBuffer {
 public:
  ArenaByteBuffer() = default;
  ~ArenaByteBuffer() = default;

  ArenaByteBuffer(const ArenaByteBuffer&) = delete;
  ArenaByteBuffer& operator=(const ArenaByteBuffer&) = delete;
  ArenaByteBuffer(ArenaByteBuffer&&) = delete;
  ArenaByteBuffer& operator=(ArenaByteBuffer&&) = delete;

  bool bind(std::span<std::byte> memory,
            std::size_t baseAlignment = 1) noexcept;

  bool bound() const noexcept { return bound_; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t remaining() const noexcept { return capacity_ - size_; }
  std::size_t baseAlignment() const noexcept { return baseAlignment_; }
  std::byte* data() noexcept { return data_; }
  const std::byte* data() const noexcept { return data_; }
  std::span<std::byte> span() noexcept { return {data_, size_}; }
  std::span<const std::byte> span() const noexcept { return {data_, size_}; }

  bool try_append(std::span<const std::byte> bytes,
                  std::size_t alignment,
                  std::size_t& offset) noexcept;

  void seal() noexcept { sealed_ = true; }
  void discard() noexcept { size_ = 0; }

 private:
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  std::size_t baseAlignment_ = 1;
  bool bound_ = false;
  bool sealed_ = false;
};

enum class SourcePayloadRegion : std::uint8_t {
  CommandHeaders,
  DrawHotStates,
  DrawShaderLayouts,
  DrawDebugSnapshots,
  DrawPsoSubviews,
  DrawUniformFixedPayloads,
  DrawUniformVertexConstants,
  DrawUniformVertexConstantBytes,
  DrawUniformPixelConstants,
  DrawUniformPixelConstantBytes,
  DrawUniformPayloads,
  DrawUniformPayloadLookupHeads,
  DrawUniformPayloadLookupTails,
  DrawUniformPayloadLookupNext,
  DrawUniformVertexConstantsLookupHeads,
  DrawUniformVertexConstantsLookupTails,
  DrawUniformVertexConstantsLookupNext,
  DrawUniformPixelConstantsLookupHeads,
  DrawUniformPixelConstantsLookupTails,
  DrawUniformPixelConstantsLookupNext,
  DrawParams,
  DrawPayloadBytes,
  DrawRunRecords,
  ClearRecords,
  ClearRects,
  SurfaceCopyRecords,
  StretchRectRecords,
  ReadbackRecords,
  ColorFillRecords,
  DepthResolveRecords,
  GenerateMipmapsRecords,
  PresentRecords,
  Count,
};

inline constexpr std::size_t kSourcePayloadRegionCount =
    static_cast<std::size_t>(SourcePayloadRegion::Count);
static_assert(kSourcePayloadRegionCount == 32);

struct SourcePayloadCapacity {
  std::size_t commandHeaders = 0;
  std::size_t drawHotStates = 0;
  std::size_t drawShaderLayouts = 0;
  std::size_t drawDebugSnapshots = 0;
  std::size_t drawPsoSubviews = 0;
  std::size_t drawUniformFixedPayloads = 0;
  std::size_t drawUniformVertexConstants = 0;
  std::size_t drawUniformVertexConstantBytes = 0;
  std::size_t drawUniformPixelConstants = 0;
  std::size_t drawUniformPixelConstantBytes = 0;
  std::size_t drawUniformPayloads = 0;
  std::size_t drawUniformPayloadLookupHeads = 0;
  std::size_t drawUniformPayloadLookupTails = 0;
  std::size_t drawUniformPayloadLookupNext = 0;
  std::size_t drawUniformVertexConstantsLookupHeads = 0;
  std::size_t drawUniformVertexConstantsLookupTails = 0;
  std::size_t drawUniformVertexConstantsLookupNext = 0;
  std::size_t drawUniformPixelConstantsLookupHeads = 0;
  std::size_t drawUniformPixelConstantsLookupTails = 0;
  std::size_t drawUniformPixelConstantsLookupNext = 0;
  std::size_t drawParams = 0;
  std::size_t drawPayloadBytes = 0;
  std::size_t drawRunRecords = 0;
  std::size_t clearRecords = 0;
  std::size_t clearRects = 0;
  std::size_t surfaceCopyRecords = 0;
  std::size_t stretchRectRecords = 0;
  std::size_t readbackRecords = 0;
  std::size_t colorFillRecords = 0;
  std::size_t depthResolveRecords = 0;
  std::size_t generateMipmapsRecords = 0;
  std::size_t presentRecords = 0;
};

// Fixed portion of ClearDesc. The nested rect vector is represented by the
// separate ClearRects region; command views and construction land in the next
// foundation slice.
struct ArenaClearRecord {
  std::array<RenderTargetAttachment, kMaxRenderTargets> colorAttachments{};
  RenderTargetAttachment depthStencil{};
  bool clearColor = false;
  bool clearDepth = false;
  bool clearStencil = false;
  ColorRGBA color{};
  f32 depth = 1.0f;
  u32 stencil = 0;
  std::uint32_t firstRect = 0;
  std::uint32_t rectCount = 0;
};
static_assert(std::is_trivially_destructible_v<ArenaClearRecord>);

struct SourcePayloadLayoutRegion {
  SourcePayloadRegion id = SourcePayloadRegion::CommandHeaders;
  std::size_t offset = 0;
  std::size_t byteCount = 0;
  std::size_t alignment = 1;
  std::size_t elementCount = 0;
  std::size_t elementSize = 0;
};

struct SourcePayloadLayout {
  std::array<SourcePayloadLayoutRegion, kSourcePayloadRegionCount> regions{};
  std::size_t usedBytes = 0;
  std::size_t pageCount = 0;
  std::size_t maximumAlignment = 1;
  std::size_t requiredBaseAlignment = 1;

  const SourcePayloadLayoutRegion& region(SourcePayloadRegion id) const noexcept {
    return regions[static_cast<std::size_t>(id)];
  }
};

inline constexpr std::size_t kMaxArenaSourcePayloadSegments = 8;

struct ArenaSourcePayloadSegmentLayout {
  SourcePayloadLayout layout{};
  std::size_t byteOffset = 0;
};

// One logical source may contain several independently indexed payload blocks
// packed into one final Tape extent. Segment boundaries are storage details:
// command indices remain logical-source-relative and completion still names
// the single source/ticket that owns the complete extent.
struct ArenaSourcePayloadLayout {
  std::array<ArenaSourcePayloadSegmentLayout,
             kMaxArenaSourcePayloadSegments> segments{};
  std::size_t segmentCount = 0;
  std::size_t usedBytes = 0;
  std::size_t pageCount = 0;

  bool valid() const noexcept {
    if (segmentCount == 0 ||
        segmentCount > kMaxArenaSourcePayloadSegments ||
        usedBytes == 0 || pageCount == 0) {
      return false;
    }
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < segmentCount; ++i) {
      const auto& segment = segments[i];
      const std::size_t alignment =
          segment.layout.requiredBaseAlignment;
      if (segment.layout.usedBytes == 0 ||
          segment.layout.pageCount == 0 || alignment == 0 ||
          (alignment & (alignment - 1)) != 0 ||
          segment.byteOffset < cursor ||
          segment.byteOffset % alignment != 0 ||
          segment.layout.usedBytes >
              std::numeric_limits<std::size_t>::max() -
                  segment.byteOffset) {
        return false;
      }
      cursor = segment.byteOffset + segment.layout.usedBytes;
    }
    return cursor == usedBytes;
  }
};

std::optional<SourcePayloadLayout> makeSourcePayloadLayout(
    const SourcePayloadCapacity& capacity,
    std::size_t pageSize,
    std::size_t maxPages = std::numeric_limits<std::size_t>::max()) noexcept;

std::optional<ArenaSourcePayloadLayout> makeArenaSourcePayloadLayout(
    std::span<const SourcePayloadLayout> segments,
    std::size_t pageSize,
    std::size_t maxPages = std::numeric_limits<std::size_t>::max()) noexcept;

class ArenaSourcePayloadBlock {
 public:
  ArenaSourcePayloadBlock() = default;
  ~ArenaSourcePayloadBlock() { destroyConstructed(); }

  ArenaSourcePayloadBlock(const ArenaSourcePayloadBlock&) = delete;
  ArenaSourcePayloadBlock& operator=(const ArenaSourcePayloadBlock&) = delete;
  ArenaSourcePayloadBlock(ArenaSourcePayloadBlock&&) = delete;
  ArenaSourcePayloadBlock& operator=(ArenaSourcePayloadBlock&&) = delete;

  bool bound() const noexcept { return bound_; }
  bool published() const noexcept { return published_; }
  bool readable() const noexcept { return published_ && !destroyed_; }
  bool boundTo(std::span<const std::byte> memory) const noexcept {
    return bound_ && boundBase_ == memory.data() &&
           boundExtent_ == memory.size();
  }
  std::size_t actualCount(SourcePayloadRegion region) const noexcept {
    return actualCounts_[static_cast<std::size_t>(region)];
  }

  // Reclaim callers invoke this after detaching the block and releasing the
  // queue lock. In particular, DrawShaderLayoutContext destruction may release
  // re-entrant resource owners.
  void destroyConstructed() noexcept;

 private:
  friend class ArenaSourcePayloadBuilder;
  friend class ArenaSourcePayloadChain;
  friend class SourcePayloadView;
  friend struct ArenaSourcePayloadBlockTestAccess;

  bool bind(std::span<std::byte> memory,
            const SourcePayloadLayout& layout) noexcept;
  bool validateForPublish() const noexcept;
  void publishCounts() noexcept;

  ArenaSoA<MetalCommandHeader> commandHeaders_{};
  ArenaSoA<FlatDrawStateRecord> drawHotStates_{};
  ArenaSoA<DrawShaderLayoutContext> drawShaderLayouts_{};
  ArenaSoA<DrawDebugSnapshot> drawDebugSnapshots_{};
  ArenaSoA<DrawPsoSubview> drawPsoSubviews_{};
  ArenaSoA<DrawUniformFixedPayloadRecord> drawUniformFixedPayloads_{};
  ArenaSoA<DrawUniformVertexConstantsRecord> drawUniformVertexConstants_{};
  ArenaByteBuffer drawUniformVertexConstantBytes_{};
  ArenaSoA<DrawUniformPixelConstantsRecord> drawUniformPixelConstants_{};
  ArenaByteBuffer drawUniformPixelConstantBytes_{};
  ArenaSoA<DrawUniformPayloadRecord> drawUniformPayloads_{};
  ArenaSoA<std::uint32_t> drawUniformPayloadLookupHeads_{};
  ArenaSoA<std::uint32_t> drawUniformPayloadLookupTails_{};
  ArenaSoA<std::uint32_t> drawUniformPayloadLookupNext_{};
  ArenaSoA<std::uint32_t> drawUniformVertexConstantsLookupHeads_{};
  ArenaSoA<std::uint32_t> drawUniformVertexConstantsLookupTails_{};
  ArenaSoA<std::uint32_t> drawUniformVertexConstantsLookupNext_{};
  ArenaSoA<std::uint32_t> drawUniformPixelConstantsLookupHeads_{};
  ArenaSoA<std::uint32_t> drawUniformPixelConstantsLookupTails_{};
  ArenaSoA<std::uint32_t> drawUniformPixelConstantsLookupNext_{};
  ArenaSoA<DrawParam> drawParams_{};
  ArenaByteBuffer drawPayloadBytes_{};
  ArenaSoA<DrawRunCommandRecord> drawRunRecords_{};
  ArenaSoA<ArenaClearRecord> clearRecords_{};
  ArenaSoA<Rect> clearRects_{};
  ArenaSoA<SurfaceCopyDesc> surfaceCopyRecords_{};
  ArenaSoA<StretchRectDesc> stretchRectRecords_{};
  ArenaSoA<ReadbackDesc> readbackRecords_{};
  ArenaSoA<ColorFillDesc> colorFillRecords_{};
  ArenaSoA<DepthResolveDesc> depthResolveRecords_{};
  ArenaSoA<GenerateMipmapsDesc> generateMipmapsRecords_{};
  ArenaSoA<PresentCommandRecord> presentRecords_{};
  std::array<std::size_t, kSourcePayloadRegionCount> actualCounts_{};
  const std::byte* boundBase_ = nullptr;
  std::size_t boundExtent_ = 0;
  bool bound_ = false;
  bool published_ = false;
  bool destroyed_ = false;
};

// Queue-owned immutable locator table for the payload blocks of one logical
// source. It owns no payload storage. initialize() is called only while the
// source is unpublished; after Ready visibility the table stays immutable
// until ordered reclaim clears it.
class ArenaSourcePayloadChain {
 public:
  bool initialize(
      std::span<const ArenaSourcePayloadBlock* const> segments) noexcept;
  void clear() noexcept;

  bool readable() const noexcept { return readable_; }
  std::size_t segmentCount() const noexcept { return segmentCount_; }
  std::size_t commandCount() const noexcept {
    return commandOffsets_[segmentCount_];
  }
  const ArenaSourcePayloadBlock* segment(std::size_t index) const noexcept {
    return index < segmentCount_ ? segments_[index] : nullptr;
  }
  bool locateCommand(std::size_t logicalCommandIndex,
                     std::size_t& segmentIndex,
                     std::size_t& localCommandIndex) const noexcept;

 private:
  std::array<const ArenaSourcePayloadBlock*,
             kMaxArenaSourcePayloadSegments> segments_{};
  std::array<std::size_t, kMaxArenaSourcePayloadSegments + 1>
      commandOffsets_{};
  std::size_t segmentCount_ = 0;
  bool readable_ = false;
};

class TransactionalChunkSlotAssembler;

class ArenaSourcePayloadBuilder {
 public:
  ArenaSourcePayloadBuilder(ArenaSourcePayloadBlock& block,
                            const SourcePayloadLayout& layout,
                            std::span<std::byte> memory) noexcept;

  ArenaSourcePayloadBuilder(const ArenaSourcePayloadBuilder&) = delete;
  ArenaSourcePayloadBuilder& operator=(const ArenaSourcePayloadBuilder&) = delete;
  ArenaSourcePayloadBuilder(ArenaSourcePayloadBuilder&&) = delete;
  ArenaSourcePayloadBuilder& operator=(ArenaSourcePayloadBuilder&&) = delete;

  bool good() const noexcept { return good_; }
  bool failed() const noexcept { return !good_; }
  bool publish() noexcept;

  bool tryAppendCommand(MetalCommandKind kind,
                        std::uint32_t payloadIndex) noexcept;
  bool tryAppendDrawHotState(const FlatDrawStateRecord& value) noexcept;
  bool tryAppendDrawShaderLayout(DrawShaderLayoutContext&& value) noexcept;
  bool tryAppendDrawShaderLayout(
      const DrawShaderLayoutContext& value) noexcept;
  bool tryAppendDrawDebugSnapshot(const DrawDebugSnapshot& value) noexcept;
  bool tryAppendDrawPsoSubview(const DrawPsoSubview& value) noexcept;
  bool tryAppendDrawUniformFixedPayload(
      const DrawUniformFixedPayloadRecord& value) noexcept;
  bool tryAppendDrawUniformVertexConstants(
      const DrawUniformVertexConstantsRecord& value) noexcept;
  bool tryAppendDrawUniformPixelConstants(
      const DrawUniformPixelConstantsRecord& value) noexcept;
  bool tryAppendDrawUniformPayload(
      const DrawUniformPayloadRecord& value) noexcept;
  bool tryAppendLookup(SourcePayloadRegion region,
                       std::uint32_t value) noexcept;
  bool tryAppendDrawParam(const DrawParam& value) noexcept;
  bool tryAppendDrawRun(const DrawRunCommandRecord& value) noexcept;
  bool tryAppendVertexConstantBytes(std::span<const u8> bytes,
                                    std::size_t alignment,
                                    std::size_t& offset) noexcept;
  bool tryAppendPixelConstantBytes(std::span<const u8> bytes,
                                   std::size_t alignment,
                                   std::size_t& offset) noexcept;
  bool tryAppendDrawPayloadBytes(std::span<const u8> bytes,
                                 std::size_t alignment,
                                 std::size_t& offset) noexcept;
  bool tryAppendClearCommand(const ClearDesc& clear) noexcept;
  bool tryAppendSurfaceCopyCommand(const SurfaceCopyDesc& value) noexcept;
  bool tryAppendStretchRectCommand(const StretchRectDesc& value) noexcept;
  bool tryAppendReadbackCommand(const ReadbackDesc& value) noexcept;
  bool tryAppendColorFillCommand(const ColorFillDesc& value) noexcept;
  bool tryAppendDepthResolveCommand(const DepthResolveDesc& value) noexcept;
  bool tryAppendGenerateMipmapsCommand(
      const GenerateMipmapsDesc& value) noexcept;
  bool tryAppendPresentCommand(PresentCommandRecord&& value) noexcept;
  bool publishValidationReady() const noexcept;
  void rollback() noexcept;
  const FlatDrawStateRecord* drawHotState(std::size_t index) const noexcept;
  const DrawShaderLayoutContext* drawShaderLayout(
      std::size_t index) const noexcept;
  DrawRunCommandRecord* drawRun(std::size_t index) noexcept;

 private:
  friend class TransactionalChunkSlotAssembler;

  bool reject() noexcept {
    good_ = false;
    return false;
  }

  template <typename T>
  bool append(ArenaSoA<T>& storage, const T& value) noexcept {
    if (!good_ || !storage.try_emplace_back(value)) {
      good_ = false;
      return false;
    }
    return true;
  }

  bool appendBytes(ArenaByteBuffer& storage,
                   std::span<const u8> bytes,
                   std::size_t alignment,
                   std::size_t& offset) noexcept;
  template <typename T>
  bool appendCommandRecord(MetalCommandKind kind,
                           ArenaSoA<T>& storage,
                           const T& value) noexcept;

  ArenaSourcePayloadBlock* block_ = nullptr;
  bool good_ = false;
};

// Replay-thread-scoped adapter from the existing DrawRunSubmission scratch
// carrier to the final fixed-capacity arena regions. It owns no storage and
// performs no fallback allocation; any partial append makes the underlying
// builder sticky-failed and the enclosing publication ticket must abort.
class TransactionalChunkSlotAssembler {
 public:
  // Value-only outer ownership witness supplied by CpuReadyArenaBuildLease.
  // The assembler cannot own the queue lease (the queue must release it after
  // publication), but it can require the lease's exact page/control/sequence
  // and resource generation before allowing final publication.
  struct OuterBinding {
    std::uint64_t rawOrdinal = 0;
    std::uint64_t sourceOrdinal = 0;
    std::uint64_t seqId = 0;
    std::uint64_t buildGeneration = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t storageGeneration = 0;
    std::uint32_t controlIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t pageCount = 0;
    std::uint32_t segmentIndex = 0;
    std::uint32_t segmentCount = 0;
    std::size_t plannedBytes = 0;

    constexpr bool valid() const noexcept {
      return rawOrdinal != 0 && sourceOrdinal != 0 && seqId != 0 &&
             buildGeneration != 0 && sourceGeneration != 0 &&
             storageGeneration != 0 &&
             controlIndex != std::numeric_limits<std::uint32_t>::max() &&
             firstPage != std::numeric_limits<std::uint32_t>::max() &&
             pageCount != 0 && segmentCount != 0 &&
             segmentIndex < segmentCount && plannedBytes != 0;
    }
  };

  enum class State : std::uint8_t {
    Empty,
    Reserved,
    Building,
    Prepared,
    Committed,
    RolledBack,
    Failed,
  };

  explicit TransactionalChunkSlotAssembler(
      ArenaSourcePayloadBuilder& builder) noexcept
      : builder_(&builder) {
    reserve();
  }

  ~TransactionalChunkSlotAssembler() {
    // A queue abort may happen after this assembler commits its local
    // construction but before CpuReadyTape visibility. In that post-effect
    // case the detached Tape owner destroys the block before this context is
    // reset; the Committed state deliberately does not double-destroy it.
    if (state_ == State::Reserved || state_ == State::Building ||
        state_ == State::Prepared || state_ == State::Failed) {
      rollback();
    }
  }

  TransactionalChunkSlotAssembler(
      const TransactionalChunkSlotAssembler&) = delete;
  TransactionalChunkSlotAssembler& operator=(
      const TransactionalChunkSlotAssembler&) = delete;
  TransactionalChunkSlotAssembler(
      TransactionalChunkSlotAssembler&&) = delete;
  TransactionalChunkSlotAssembler& operator=(
      TransactionalChunkSlotAssembler&&) = delete;

  bool reserve() noexcept;
  bool bindOuter(OuterBinding binding) noexcept;
  // Publishes the immutable block for validation/planning. This is the only
  // fallible part of the production transaction after construction.
  bool prepare() noexcept;
  // Only CommandQueue can mint the evidence passed here.
  bool bindCommitEvidence(ArenaCommitEvidence&& evidence) noexcept;
  // Native value-only fixtures that do not own a queue lease must opt into
  // this explicitly; production queue code always uses evidence-gated commit.
  bool commitValueOnlyForTest() noexcept;
  bool commit() noexcept;
  void rollback() noexcept;

  template <typename Build>
    requires std::is_nothrow_invocable_r_v<
        bool, Build&, TransactionalChunkSlotAssembler&>
  bool build(Build&& buildFn) noexcept {
    if (!beginBuild() || !buildFn(*this)) {
      fail();
      return false;
    }
    return true;
  }

  State state() const noexcept { return state_; }
  bool good() const noexcept {
    return builder_ && builder_->good() &&
           (state_ == State::Reserved || state_ == State::Building);
  }
  bool commitEvidenceMatches(
      const void* liveTransaction, std::uint64_t rawOrdinal,
      std::uint64_t sourceOrdinal, std::uint64_t seqId,
      std::uint64_t buildGeneration, std::uint64_t sourceGeneration,
      std::uint64_t storageGeneration, std::uint32_t controlIndex,
      std::uint32_t firstPage, std::uint32_t pageCount,
      std::uint32_t segmentIndex, std::uint32_t segmentCount,
      std::size_t plannedBytes, std::uint64_t resourceCount,
      std::uint64_t resourceDigest) const noexcept {
    return commitEvidence_.has_value() &&
           commitEvidence_->matches(
               liveTransaction, rawOrdinal, sourceOrdinal, seqId,
               buildGeneration, sourceGeneration, storageGeneration,
               controlIndex, firstPage, pageCount, segmentIndex, segmentCount,
               plannedBytes, resourceCount, resourceDigest);
  }
  bool failed() const noexcept { return !good(); }
  std::size_t commandCount() const noexcept { return commandCount_; }
  const std::optional<OuterBinding>& outerBinding() const noexcept {
    return outerBinding_;
  }

  bool tryAppendDrawRunBatch(
      std::span<DrawRunSubmission> submissions) noexcept;
  bool tryAppendDirectDraw(
      const DirectReplayDrawInput& input) noexcept;
  bool tryAppendClear(const ClearDesc& value) noexcept;
  bool tryAppendSurfaceCopy(const SurfaceCopyDesc& value) noexcept;
  bool tryAppendStretchRect(const StretchRectDesc& value) noexcept;
  bool tryAppendReadback(const ReadbackDesc& value) noexcept;
  bool tryAppendColorFill(const ColorFillDesc& value) noexcept;
  bool tryAppendDepthResolve(const DepthResolveDesc& value) noexcept;
  bool tryAppendGenerateMipmaps(const GenerateMipmapsDesc& value) noexcept;
  bool tryAppendPresent(PresentCommandRecord&& value) noexcept;

 private:
  bool beginBuild() noexcept;
  bool fail() noexcept;
  bool tryAppendUniform(const DrawUniformPayload& payload,
                        DrawUniformHandle& handle) noexcept;
  bool tryAppendPayloadBytes(std::span<const u8> bytes,
                             DrawPayloadRange& range,
                             std::size_t runPayloadOffset) noexcept;

  ArenaSourcePayloadBuilder* builder_ = nullptr;
  std::size_t commandCount_ = 0;
  std::size_t stateCount_ = 0;
  std::size_t uniformCount_ = 0;
  std::size_t paramCount_ = 0;
  std::size_t drawRunCount_ = 0;
  std::size_t drawPayloadBytes_ = 0;
  std::size_t vertexConstantBytes_ = 0;
  std::size_t pixelConstantBytes_ = 0;
  std::size_t directRunStateIndex_ = 0;
  std::size_t directRunRecordIndex_ = 0;
  std::size_t directRunPayloadOffset_ = 0;
  bool directRunOpen_ = false;
  std::optional<OuterBinding> outerBinding_{};
  std::optional<ArenaCommitEvidence> commitEvidence_{};
  State state_ = State::Empty;
};

static_assert(std::is_trivially_copyable_v<
              TransactionalChunkSlotAssembler::OuterBinding>);

// Compatibility spelling for existing callers while the production owner is
// migrated to the explicit transaction name.
using ArenaSourcePayloadAssembler = TransactionalChunkSlotAssembler;

struct ClearCommandView {
  std::array<RenderTargetAttachment, kMaxRenderTargets> colorAttachments{};
  RenderTargetAttachment depthStencil{};
  bool clearColor = false;
  bool clearDepth = false;
  bool clearStencil = false;
  ColorRGBA color{};
  f32 depth = 1.0f;
  u32 stencil = 0;
  std::span<const Rect> rects{};
};

struct SourceCommandView {
  MetalCommandView command{};
  std::optional<ClearCommandView> clear{};
  std::uint32_t payloadIndex = 0;
  std::size_t segmentIndex = 0;
  std::size_t localCommandIndex = 0;

  MetalCommandKind kind() const noexcept { return command.kind; }
};

struct SourceCommandResourceRef {
  ChunkHandleEntry entry{};
  DrawBufferBindingSnapshot bufferSnapshot{};
  bool hasBufferSnapshot = false;
};

// Walk the production resource set for one command. Queue residency marking
// and effective-replay observation share this policy so flat draw state,
// per-draw binding overrides/snapshots, and non-draw endpoints cannot drift.
// Snapshot-bearing rows preserve the concrete backing stamp required by
// DYNAMIC-buffer lifetime marking; observers expose only `entry`.
template <typename Visitor>
void visitSourceCommandResources(const SourceCommandView& source,
                                 Visitor&& visitor) {
  const auto emit = [&](ChunkHandleKind kind, Handle handle,
                        DrawBufferBindingSnapshot snapshot = {},
                        bool hasBufferSnapshot = false) {
    if (handle) {
      std::invoke(visitor, SourceCommandResourceRef{
                               .entry = {.kind = kind, .handle = handle},
                               .bufferSnapshot = snapshot,
                               .hasBufferSnapshot = hasBufferSnapshot,
                           });
    }
  };

  switch (source.command.kind) {
  case MetalCommandKind::DrawRun:
    if (source.command.drawState.hot) {
      const auto& hot = *source.command.drawState.hot;
      emit(ChunkHandleKind::Buffer, hot.indexBuffer);
      for (const auto handle : hot.streamBuffers) {
        emit(ChunkHandleKind::Buffer, handle);
      }
      for (const auto handle : hot.textures) {
        emit(ChunkHandleKind::Texture, handle);
      }
      for (const auto& attachment : hot.colorAttachments) {
        emit(ChunkHandleKind::Surface, attachment.handle);
      }
      emit(ChunkHandleKind::Surface, hot.depthStencil.handle);
    }
    {
      const auto arena = drawRunPayloadBytes(source.command);
      for (const auto& param : source.command.drawParams) {
        const auto overrideBytes =
            drawRunPayloadBytes(param.bindingOverrideRange, arena);
        if (overrideBytes.size() == sizeof(DrawBindingOverride)) {
          DrawBindingOverride binding{};
          std::memcpy(&binding, overrideBytes.data(), sizeof(binding));
          for (u32 stream = 0; stream < kMaxStreams; ++stream) {
            if ((binding.streamMask & (1u << stream)) != 0u) {
              emit(ChunkHandleKind::Buffer,
                   binding.streams[stream].buffer);
            }
          }
          if (binding.indexBufferValid) {
            emit(ChunkHandleKind::Buffer, binding.indexBuffer);
          }
        }

        const auto snapshotBytes =
            drawRunPayloadBytes(param.bindingSnapshotRange, arena);
        if (snapshotBytes.size() == sizeof(DrawBindingSnapshot)) {
          DrawBindingSnapshot binding{};
          std::memcpy(&binding, snapshotBytes.data(), sizeof(binding));
          for (u32 stream = 0; stream < kMaxStreams; ++stream) {
            if ((binding.streamMask & (1u << stream)) != 0u) {
              emit(ChunkHandleKind::Buffer,
                   binding.streams[stream].buffer,
                   binding.streams[stream].snapshot,
                   /*hasBufferSnapshot=*/true);
            }
          }
          if (binding.indexSnapshotValid) {
            emit(ChunkHandleKind::Buffer, binding.indexBuffer,
                 binding.indexSnapshot,
                 /*hasBufferSnapshot=*/true);
          }
        }
      }
    }
    break;
  case MetalCommandKind::Clear:
    if (source.clear) {
      const auto& clear = *source.clear;
      if (clear.clearColor) {
        for (const auto& attachment : clear.colorAttachments) {
          emit(ChunkHandleKind::Surface, attachment.handle);
        }
      }
      if (clear.clearDepth || clear.clearStencil) {
        emit(ChunkHandleKind::Surface, clear.depthStencil.handle);
      }
    } else if (source.command.clear) {
      const auto& clear = *source.command.clear;
      if (clear.clearColor) {
        for (const auto& attachment : clear.colorAttachments) {
          emit(ChunkHandleKind::Surface, attachment.handle);
        }
      }
      if (clear.clearDepth || clear.clearStencil) {
        emit(ChunkHandleKind::Surface, clear.depthStencil.handle);
      }
    }
    break;
  case MetalCommandKind::SurfaceCopy:
    if (source.command.surfaceCopy) {
      emit(ChunkHandleKind::Surface, source.command.surfaceCopy->source);
      emit(ChunkHandleKind::Surface,
           source.command.surfaceCopy->destination);
    }
    break;
  case MetalCommandKind::StretchRect:
    if (source.command.stretchRect) {
      emit(ChunkHandleKind::Surface, source.command.stretchRect->source);
      emit(ChunkHandleKind::Surface,
           source.command.stretchRect->destination);
    }
    break;
  case MetalCommandKind::Readback:
    if (source.command.readback) {
      emit(ChunkHandleKind::Surface, source.command.readback->source);
      emit(ChunkHandleKind::Surface,
           source.command.readback->destination);
    }
    break;
  case MetalCommandKind::ColorFill:
    if (source.command.colorFill) {
      emit(ChunkHandleKind::Surface, source.command.colorFill->destination);
    }
    break;
  case MetalCommandKind::DepthResolve:
    if (source.command.depthResolve) {
      emit(ChunkHandleKind::Surface, source.command.depthResolve->msaaDepth);
      emit(ChunkHandleKind::Surface, source.command.depthResolve->intzDest);
    }
    break;
  case MetalCommandKind::GenerateMipmaps:
    if (source.command.generateMipmaps) {
      emit(ChunkHandleKind::Texture,
           source.command.generateMipmaps->texture);
    }
    break;
  case MetalCommandKind::Present:
    if (source.command.present) {
      emit(ChunkHandleKind::Surface, source.command.present->presentSource);
    }
    break;
  }
}

// This is a synchronous, call-local borrowed view. Never store it or any span
// returned from it in a session, partition, submission, or callback. Every
// access must stay inside either Ready-prefix selection under the queue lock or
// a synchronous encode call protected by the source's Represented pin.
class SourcePayloadView {
 public:
  SourcePayloadView() = default;
  explicit SourcePayloadView(const ChunkSlot& legacy) noexcept
      : legacy_(&legacy) {}
  explicit SourcePayloadView(const ArenaSourcePayloadBlock& arena) noexcept
      : arena_(arena.readable() ? &arena : nullptr) {}
  explicit SourcePayloadView(const ArenaSourcePayloadChain& arena) noexcept
      : arenaChain_(arena.readable() ? &arena : nullptr) {}

  bool valid() const noexcept {
    return legacy_ != nullptr || arena_ != nullptr || arenaChain_ != nullptr;
  }
  bool isLegacy() const noexcept { return legacy_ != nullptr; }
  bool isArena() const noexcept {
    return arena_ != nullptr || arenaChain_ != nullptr;
  }
  const ChunkSlot* legacyPayload() const noexcept { return legacy_; }
  const ArenaSourcePayloadBlock* arenaPayload() const noexcept {
    return arena_ ? arena_
                  : arenaChain_ && arenaChain_->segmentCount() == 1
                        ? arenaChain_->segment(0)
                        : nullptr;
  }
  std::size_t arenaSegmentCount() const noexcept {
    return arena_ ? 1u : arenaChain_ ? arenaChain_->segmentCount() : 0u;
  }
  SourcePayloadView arenaSegment(std::size_t index) const noexcept {
    if (arena_ && index == 0) {
      return SourcePayloadView(*arena_);
    }
    const auto* segment = arenaChain_ ? arenaChain_->segment(index) : nullptr;
    return segment ? SourcePayloadView(*segment) : SourcePayloadView{};
  }
  bool locateCommand(std::size_t logicalCommandIndex,
                     std::size_t& segmentIndex,
                     std::size_t& localCommandIndex) const noexcept;
  friend bool operator==(const SourcePayloadView&,
                         const SourcePayloadView&) = default;
  std::size_t commandCount() const noexcept;
  bool commandsEmpty() const noexcept { return commandCount() == 0; }
  std::size_t presentRecordCount() const noexcept;
  bool drawOnlyCommandStream() const noexcept;
  SourceCommandView commandAt(std::size_t index) const noexcept;

  std::span<const DrawUniformPayloadRecord> drawUniformPayloads() const noexcept;
  std::span<const u8> drawUniformVertexConstantBytes() const noexcept;
  std::span<const u8> drawUniformPixelConstantBytes() const noexcept;
  std::span<const DrawParam> drawParams() const noexcept;
  std::span<const u8> drawPayloadBytes() const noexcept;

 private:
  const ChunkSlot* legacy_ = nullptr;
  const ArenaSourcePayloadBlock* arena_ = nullptr;
  const ArenaSourcePayloadChain* arenaChain_ = nullptr;
};

}  // namespace dxmt9::core
