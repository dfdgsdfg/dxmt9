#pragma once

#include "dxmt9_backend_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9::core {

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
  PresentRecords,
  Count,
};

inline constexpr std::size_t kSourcePayloadRegionCount =
    static_cast<std::size_t>(SourcePayloadRegion::Count);
static_assert(kSourcePayloadRegionCount == 31);

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

std::optional<SourcePayloadLayout> makeSourcePayloadLayout(
    const SourcePayloadCapacity& capacity,
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
  ArenaSoA<PresentCommandRecord> presentRecords_{};
  std::array<std::size_t, kSourcePayloadRegionCount> actualCounts_{};
  const std::byte* boundBase_ = nullptr;
  std::size_t boundExtent_ = 0;
  bool bound_ = false;
  bool published_ = false;
  bool destroyed_ = false;
};

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
  bool tryAppendPresentCommand(PresentCommandRecord&& value) noexcept;

 private:
  friend class ArenaSourcePayloadAssembler;

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
class ArenaSourcePayloadAssembler {
 public:
  explicit ArenaSourcePayloadAssembler(
      ArenaSourcePayloadBuilder& builder) noexcept
      : builder_(&builder) {}

  ArenaSourcePayloadAssembler(const ArenaSourcePayloadAssembler&) = delete;
  ArenaSourcePayloadAssembler& operator=(
      const ArenaSourcePayloadAssembler&) = delete;

  bool good() const noexcept { return builder_ && builder_->good(); }
  bool failed() const noexcept { return !good(); }
  std::size_t commandCount() const noexcept { return commandCount_; }

  bool tryAppendDrawRunBatch(
      std::span<DrawRunSubmission> submissions) noexcept;
  bool tryAppendClear(const ClearDesc& value) noexcept;
  bool tryAppendSurfaceCopy(const SurfaceCopyDesc& value) noexcept;
  bool tryAppendStretchRect(const StretchRectDesc& value) noexcept;
  bool tryAppendColorFill(const ColorFillDesc& value) noexcept;
  bool tryAppendDepthResolve(const DepthResolveDesc& value) noexcept;

 private:
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
};

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

  MetalCommandKind kind() const noexcept { return command.kind; }
};

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

  bool valid() const noexcept { return legacy_ != nullptr || arena_ != nullptr; }
  bool isLegacy() const noexcept { return legacy_ != nullptr; }
  bool isArena() const noexcept { return arena_ != nullptr; }
  const ChunkSlot* legacyPayload() const noexcept { return legacy_; }
  const ArenaSourcePayloadBlock* arenaPayload() const noexcept { return arena_; }
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
};

}  // namespace dxmt9::core
