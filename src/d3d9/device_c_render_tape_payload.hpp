#pragma once

#include "device_c_render_tape_snapshot_budget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dxmt9::d3d9 {

class RenderTapeCpuShadowContent;

inline constexpr std::size_t kRenderTapePayloadWindowBytes = 64u * 1024u;

class RenderTapePayloadReadError : public std::exception {
public:
  const char* what() const noexcept override { return "render-tape scratch I/O failed"; }
};

// An injectable I/O boundary, used by the deterministic fault fixture. The
// production functions use the CRT in both PE and native builds; no mapping or
// host pointer becomes part of a PE payload's representation.
struct RenderTapeScratchIo {
  static std::FILE* openDefault() { return std::tmpfile(); }
  static bool seekDefault(std::FILE* file, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
#if defined(_WIN32)
    return _fseeki64(file, static_cast<std::int64_t>(offset), SEEK_SET) == 0;
#else
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
  }
  static std::size_t readDefault(std::FILE* file, void* bytes, std::size_t size) {
    return std::fread(bytes, 1u, size, file);
  }
  static std::size_t writeDefault(std::FILE* file, const void* bytes, std::size_t size) {
    return std::fwrite(bytes, 1u, size, file);
  }
  static bool flushDefault(std::FILE* file) { return std::fflush(file) == 0; }
  static void closeDefault(std::FILE* file) { (void)std::fclose(file); }
  std::FILE* (*open)() = openDefault;
  bool (*seek)(std::FILE*, std::uint64_t) = seekDefault;
  std::size_t (*read)(std::FILE*, void*, std::size_t) = readDefault;
  std::size_t (*write)(std::FILE*, const void*, std::size_t) = writeDefault;
  bool (*flush)(std::FILE*) = flushDefault;
  void (*close)(std::FILE*) = closeDefault;
};

// One anonymous, delete-on-close file per owner scope, rather than one open
// descriptor per captured version. Retired nodes themselves form the free
// list, so releasing the last immutable lease never allocates. A failed stage
// retires its extent exactly as a successfully committed version does.
class RenderTapeScratchArena {
public:
  struct Extent {
    std::uint64_t offset = 0u;
    std::uint64_t bytes = 0u;
    std::unique_ptr<Extent> next{};
  };
  explicit RenderTapeScratchArena(RenderTapeScratchIo io = {}) noexcept : io_(io) {}
  ~RenderTapeScratchArena() { if (file_) io_.close(file_); }
  RenderTapeScratchArena(const RenderTapeScratchArena&) = delete;
  RenderTapeScratchArena& operator=(const RenderTapeScratchArena&) = delete;

  std::unique_ptr<Extent> reserve(std::uint64_t bytes) {
    if (bytes == 0u) throw std::invalid_argument("empty render-tape scratch extent");
    auto extent = std::make_unique<Extent>();
    std::lock_guard lock(mutex_);
    if (!file_) {
      file_ = io_.open();
      if (!file_) throw RenderTapePayloadReadError();
      // A short write or failed flush must not leave queued bytes that could
      // later be replayed into an already retired/reused extent.
      if (std::setvbuf(file_, nullptr, _IONBF, 0u) != 0) {
        io_.close(file_);
        file_ = nullptr;
        throw RenderTapePayloadReadError();
      }
    }
    auto* link = &free_;
    while (*link && (*link)->bytes < bytes) link = &(*link)->next;
    if (*link) {
      extent->offset = (*link)->offset;
      extent->bytes = bytes;
      (*link)->offset += bytes;
      (*link)->bytes -= bytes;
      if ((*link)->bytes == 0u) *link = std::move((*link)->next);
    } else {
      const auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      if (bytes > max || highWater_ > max - bytes)
        throw std::length_error("render-tape scratch extent overflow");
      extent->offset = highWater_;
      extent->bytes = bytes;
      highWater_ += bytes;
    }
    liveBytes_ += bytes;
    return extent;
  }
  void retire(std::unique_ptr<Extent> extent) noexcept {
    if (!extent) return;
    std::lock_guard lock(mutex_);
    liveBytes_ -= extent->bytes;
    auto* link = &free_;
    while (*link && (*link)->offset < extent->offset) link = &(*link)->next;
    extent->next = std::move(*link);
    *link = std::move(extent);
    // Sorted adjacent holes coalesce without new metadata or I/O. The file's
    // physical high-water remains explicit; reusable holes are not truncation.
    auto* current = free_.get();
    while (current && current->next) {
      if (current->offset + current->bytes == current->next->offset) {
        current->bytes += current->next->bytes;
        current->next = std::move(current->next->next);
      } else current = current->next.get();
    }
  }
  bool read(const Extent& extent, std::uint64_t offset,
            std::span<std::byte> bytes) const noexcept {
    if (offset > extent.bytes || bytes.size() > extent.bytes - offset) return false;
    if (bytes.empty()) return true;
    std::lock_guard lock(mutex_);
    std::clearerr(file_);
    return io_.seek(file_, extent.offset + offset) &&
        io_.read(file_, bytes.data(), bytes.size()) == bytes.size();
  }
  bool write(const Extent& extent, std::uint64_t offset,
             std::span<const std::byte> bytes) noexcept {
    if (offset > extent.bytes || bytes.size() > extent.bytes - offset) return false;
    if (bytes.empty()) return true;
    std::lock_guard lock(mutex_);
    std::clearerr(file_);
    return io_.seek(file_, extent.offset + offset) &&
        io_.write(file_, bytes.data(), bytes.size()) == bytes.size();
  }
  bool flush() noexcept {
    std::lock_guard lock(mutex_);
    return file_ && io_.flush(file_);
  }
  std::uint64_t liveBytes() const noexcept { std::lock_guard lock(mutex_); return liveBytes_; }
  std::uint64_t highWaterBytes() const noexcept { std::lock_guard lock(mutex_); return highWater_; }
private:
  RenderTapeScratchIo io_{};
  std::FILE* file_ = nullptr;
  mutable std::mutex mutex_{};
  std::unique_ptr<Extent> free_{};
  std::uint64_t liveBytes_ = 0u;
  std::uint64_t highWater_ = 0u;
};

// Call-local reader. A borrowed span is consumed immediately; a scratch view
// borrows its immutable owner, never a mapping or a retained read cache.
class RenderTapeByteView {
public:
  using Read = bool (*)(const void*, std::uint64_t, std::span<std::byte>) noexcept;
  RenderTapeByteView() noexcept = default;
  RenderTapeByteView(std::span<const std::byte> bytes) noexcept : borrowed_(bytes), size_(bytes.size()) {}
  RenderTapeByteView(const void* context, std::size_t size, Read read) noexcept
      : context_(context), read_(read), size_(size) {}
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0u; }
  const std::byte* data() const noexcept { return read_ ? nullptr : borrowed_.data(); }
  bool readAt(std::uint64_t offset, std::span<std::byte> bytes) const noexcept {
    if (offset > size_ || bytes.size() > size_ - offset) return false;
    if (read_) return read_(context_, offset, bytes);
    if (!bytes.empty()) std::memcpy(bytes.data(), borrowed_.data() + offset, bytes.size());
    return true;
  }
  std::byte operator[](std::size_t offset) const {
    std::byte value{};
    if (!readAt(offset, std::span(&value, 1u))) throw RenderTapePayloadReadError();
    return value;
  }
  bool equals(RenderTapeByteView other) const {
    if (size_ != other.size_) return false;
    std::array<std::byte, kRenderTapePayloadWindowBytes> a{}, b{};
    for (std::size_t offset = 0u; offset < size_;) {
      const auto count = std::min(a.size(), size_ - offset);
      if (!readAt(offset, std::span(a).first(count)) ||
          !other.readAt(offset, std::span(b).first(count))) throw RenderTapePayloadReadError();
      if (!std::equal(a.begin(), a.begin() + count, b.begin())) return false;
      offset += count;
    }
    return true;
  }
  struct Iterator {
    using iterator_category = std::input_iterator_tag;
    using value_type = std::byte;
    using difference_type = std::ptrdiff_t;
    using reference = std::byte;
    using pointer = void;
    struct Window {
      std::array<std::byte, kRenderTapePayloadWindowBytes> bytes{};
      std::size_t offset = std::numeric_limits<std::size_t>::max();
      std::size_t count = 0u;
    };
    const void* context = nullptr;
    Read read = nullptr;
    std::span<const std::byte> borrowed{};
    std::size_t size = 0u;
    std::size_t offset = 0u;
    mutable std::shared_ptr<Window> window{};
    std::byte operator*() const {
      if (!read) return borrowed[offset];
      if (!window) window = std::make_shared<Window>();
      if (offset < window->offset || offset - window->offset >= window->count) {
        const auto count = std::min(window->bytes.size(), size - offset);
        window->count = 0u;
        if (!read(context, offset, std::span(window->bytes).first(count)))
          throw RenderTapePayloadReadError();
        window->offset = offset;
        window->count = count;
      }
      return window->bytes[offset - window->offset];
    }
    Iterator& operator++() noexcept { ++offset; return *this; }
    Iterator operator++(int) { auto prior = *this; ++*this; return prior; }
    friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
      return a.context == b.context && a.borrowed.data() == b.borrowed.data() && a.offset == b.offset;
    }
  };
  Iterator begin() const noexcept { return {context_, read_, borrowed_, size_, 0u}; }
  Iterator end() const noexcept { return {context_, read_, borrowed_, size_, size_}; }
private:
  std::span<const std::byte> borrowed_{};
  const void* context_ = nullptr;
  Read read_ = nullptr;
  std::size_t size_ = 0u;
};

// Capture-only immutable version. Raw GPU arm vectors preserve their consuming
// ownership path; CPU shadow versions own an unmapped scratch extent instead.
class RenderTapeImmutablePayload {
  struct Storage {
    Storage() noexcept = default;
    explicit Storage(std::vector<std::byte>&& input,
                     const RenderTapeSnapshotBudgetLease& lease = {}) noexcept
        : retainedLease(lease), bytes(std::move(input)) {}
    ~Storage() { if (arena) arena->retire(std::move(extent)); }
    // Reverse member destruction frees the raw payload before returning B.
    RenderTapeSnapshotBudgetLease retainedLease{};
    std::vector<std::byte> bytes;
    std::shared_ptr<RenderTapeScratchArena> arena{};
    std::unique_ptr<RenderTapeScratchArena::Extent> extent{};
    bool readAt(std::uint64_t offset, std::span<std::byte> output) const noexcept {
      return arena && extent ? arena->read(*extent, offset, output)
          : RenderTapeByteView(std::span<const std::byte>(bytes)).readAt(offset, output);
    }
  };
  friend class RenderTapeCpuShadowContent;
public:
  RenderTapeImmutablePayload() noexcept = default;
  RenderTapeImmutablePayload(std::vector<std::byte>&& bytes)
      : owner_(std::make_shared<Storage>(std::move(bytes))) {}
  RenderTapeImmutablePayload(std::vector<std::byte>&& bytes,
                             const RenderTapeSnapshotBudgetLease& retainedLease) {
    // Reject before consuming bytes or their caller-owned reservation. The
    // metadata allocation also precedes the move; adoption never reserves B.
    if (retainedLease.valid() &&
        (retainedLease.kind() != RenderTapeSnapshotBudgetKind::Retained ||
         retainedLease.resourceBytes() != bytes.size() ||
         retainedLease.chargedBytes() != bytes.size() ||
         !retainedLease.boundPayloadMatches(bytes.data(), bytes.size())))
      throw std::invalid_argument("render-tape retained snapshot lease mismatch");
    owner_ = std::make_shared<Storage>(std::move(bytes), retainedLease);
  }
  RenderTapeImmutablePayload(const std::vector<std::byte>& bytes)
      : RenderTapeImmutablePayload(std::vector<std::byte>(bytes)) {}

  RenderTapeByteView readBytes() const noexcept {
    if (!owner_) return {};
    if (!owner_->extent) return RenderTapeByteView(std::span<const std::byte>(owner_->bytes));
    return {owner_.get(), size(), [](const void* storage, std::uint64_t offset,
                                   std::span<std::byte> output) noexcept {
      return static_cast<const Storage*>(storage)->readAt(offset, output);
    }};
  }
  bool readAt(std::uint64_t offset, std::span<std::byte> output) const noexcept {
    return readBytes().readAt(offset, output);
  }
  const std::byte* data() const noexcept { return readBytes().data(); }
  const void* backingIdentity() const noexcept { return owner_.get(); }
  std::size_t size() const noexcept { return owner_ && owner_->extent
      ? static_cast<std::size_t>(owner_->extent->bytes) : owner_ ? owner_->bytes.size() : 0u; }
  bool empty() const noexcept { return size() == 0u; }
  auto begin() const noexcept { return readBytes().begin(); }
  auto end() const noexcept { return readBytes().end(); }
  std::size_t capacity() const noexcept { return owner_ ? owner_->bytes.capacity() : 0u; }
  bool hasBacking() const noexcept { return static_cast<bool>(owner_); }
  bool scratchBacked() const noexcept { return owner_ && static_cast<bool>(owner_->extent); }
  bool shared() const noexcept { return owner_ && owner_.use_count() > 1; }

  friend bool operator==(const RenderTapeImmutablePayload& a,
                         const RenderTapeImmutablePayload& b) {
    return a.readBytes().equals(b.readBytes());
  }
  friend bool operator==(const RenderTapeImmutablePayload& a,
                         const std::vector<std::byte>& b) {
    return a.readBytes().equals(RenderTapeByteView(std::span<const std::byte>(b)));
  }
  static RenderTapeImmutablePayload copyToScratch(
      RenderTapeByteView bytes, std::shared_ptr<RenderTapeScratchArena> arena) {
    if (bytes.empty()) return {};
    if (!arena) arena = std::make_shared<RenderTapeScratchArena>();
    RenderTapeImmutablePayload result;
    result.owner_ = std::make_shared<Storage>();
    result.owner_->arena = std::move(arena);
    result.owner_->extent = result.owner_->arena->reserve(bytes.size());
    std::array<std::byte, kRenderTapePayloadWindowBytes> window{};
    for (std::size_t offset = 0u; offset < bytes.size();) {
      const auto count = std::min(window.size(), bytes.size() - offset);
      if (!bytes.readAt(offset, std::span(window).first(count)) ||
          !result.owner_->arena->write(*result.owner_->extent, offset,
                                      std::span(window).first(count)))
        throw RenderTapePayloadReadError();
      offset += count;
    }
    if (!result.owner_->arena->flush()) throw RenderTapePayloadReadError();
    return result;
  }
private:
  std::shared_ptr<Storage> owner_{};
};

struct RenderTapeShadowWritePlan {
  std::uint64_t expectedBytes = 0u;
  std::uint64_t destinationOffset = 0u;
  std::uint64_t rowBytes = 0u;
  std::uint64_t rowPitch = 0u;
  std::uint64_t rows = 1u;
  std::uint64_t slicePitch = 0u;
  std::uint64_t slices = 1u;
  bool fullOverwrite = false;
};

enum class RenderTapeShadowWriteStatus : std::uint8_t {
  Accepted,
  InvalidInput,
  IncompleteContent,
  AllocationFailed,
  IoFailed,
};

// Durable CPU A is an immutable scratch version, separate from GPU arm B.
// Every writer stages another extent and commits only after complete I/O.
class RenderTapeCpuShadowContent {
public:
  RenderTapeCpuShadowContent() noexcept = default;
  explicit RenderTapeCpuShadowContent(std::vector<std::byte>&& bytes)
      : frozen_(RenderTapeImmutablePayload::copyToScratch(
          RenderTapeByteView(std::span<const std::byte>(bytes)), {})) {
    std::vector<std::byte>{}.swap(bytes);
  }
  explicit RenderTapeCpuShadowContent(RenderTapeImmutablePayload payload)
      : frozen_(payload.scratchBacked() || payload.empty() ? std::move(payload)
          : RenderTapeImmutablePayload::copyToScratch(payload.readBytes(), {})) {}
  RenderTapeCpuShadowContent(const RenderTapeCpuShadowContent&) = delete;
  RenderTapeCpuShadowContent& operator=(const RenderTapeCpuShadowContent&) = delete;
  RenderTapeCpuShadowContent(RenderTapeCpuShadowContent&&) noexcept = default;
  RenderTapeCpuShadowContent& operator=(RenderTapeCpuShadowContent&&) noexcept = default;

  RenderTapeByteView readBytes() const noexcept { return frozen_.readBytes(); }
  const void* backingIdentity() const noexcept { return frozen_.backingIdentity(); }
  void setPayloadArena(std::shared_ptr<RenderTapeScratchArena> arena) noexcept { arena_ = std::move(arena); }
  std::size_t size() const noexcept { return readBytes().size(); }
  bool empty() const noexcept { return readBytes().empty(); }
  std::size_t capacity() const noexcept {
    return frozen_.capacity();
  }
  bool shared() const noexcept { return frozen_.shared(); }

  RenderTapeImmutablePayload freeze() {
    if (writing_)
      throw std::logic_error("render-tape CPU mutation is active");
    if (empty())
      throw std::invalid_argument("cannot freeze unknown render-tape CPU content");
    return frozen_;
  }

  RenderTapeShadowWriteStatus write(
      const RenderTapeShadowWritePlan& plan, std::span<const std::byte> bytes) noexcept {
    if (writing_) return RenderTapeShadowWriteStatus::InvalidInput;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto mul = [&](std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
      if (b != 0u && a > max / b) return false;
      out = a * b;
      return true;
    };
    const auto add = [&](std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
      if (b > max - a) return false;
      out = a + b;
      return true;
    };
    std::uint64_t sourceSlice = 0u, sourceBytes = 0u, rowTail = 0u, sliceTail = 0u;
    std::uint64_t end = plan.destinationOffset;
    if (plan.expectedBytes == 0u || plan.expectedBytes > std::numeric_limits<std::size_t>::max() ||
        plan.rowBytes == 0u || plan.rows == 0u || plan.slices == 0u ||
        !mul(plan.rowBytes, plan.rows, sourceSlice) ||
        !mul(sourceSlice, plan.slices, sourceBytes) || bytes.size() != sourceBytes ||
        !mul(plan.rowPitch, plan.rows - 1u, rowTail) ||
        !mul(plan.slicePitch, plan.slices - 1u, sliceTail) ||
        !add(end, rowTail, end) || !add(end, sliceTail, end) ||
        !add(end, plan.rowBytes, end) || end > plan.expectedBytes ||
        (plan.fullOverwrite && (plan.destinationOffset != 0u || sourceBytes != plan.expectedBytes ||
            (plan.rows > 1u && plan.rowPitch != plan.rowBytes) ||
            (plan.slices > 1u && plan.slicePitch != sourceSlice))))
      return RenderTapeShadowWriteStatus::InvalidInput;
    if (!plan.fullOverwrite && size() != plan.expectedBytes)
      return RenderTapeShadowWriteStatus::IncompleteContent;

    struct WriteScope {
      bool& active;
      ~WriteScope() { active = false; }
    };
    writing_ = true;
    const WriteScope scope{writing_};

    try {
      auto arena = arena_ ? arena_ : frozen_.owner_ ? frozen_.owner_->arena : nullptr;
      if (!arena) arena = std::make_shared<RenderTapeScratchArena>();
      RenderTapeImmutablePayload replacement;
      if (plan.fullOverwrite) {
        replacement = RenderTapeImmutablePayload::copyToScratch(RenderTapeByteView(bytes), arena);
      } else {
        replacement = RenderTapeImmutablePayload::copyToScratch(readBytes(), arena);
        for (std::uint64_t slice = 0u; slice < plan.slices; ++slice)
          for (std::uint64_t row = 0u; row < plan.rows; ++row)
            if (!arena->write(*replacement.owner_->extent,
                    plan.destinationOffset + slice * plan.slicePitch + row * plan.rowPitch,
                    bytes.subspan(static_cast<std::size_t>(slice * sourceSlice + row * plan.rowBytes),
                                  static_cast<std::size_t>(plan.rowBytes))))
              throw RenderTapePayloadReadError();
        if (!arena->flush()) throw RenderTapePayloadReadError();
      }
      frozen_ = std::move(replacement);
      arena_ = std::move(arena);
      return RenderTapeShadowWriteStatus::Accepted;
    } catch (const RenderTapePayloadReadError&) {
      return RenderTapeShadowWriteStatus::IoFailed;
    } catch (...) {
      return RenderTapeShadowWriteStatus::AllocationFailed;
    }
  }
private:
  RenderTapeImmutablePayload frozen_{};
  std::shared_ptr<RenderTapeScratchArena> arena_{};
  bool writing_ = false;
};

// Read-only PE/admission adapter. This is not a mutable vector replacement;
// arm snapshots remain raw vectors and durable writes stay explicit above.
class RenderTapeContentView {
public:
  RenderTapeContentView() noexcept = default;
  RenderTapeContentView(std::span<const std::vector<std::byte>> content) noexcept
      : vectors_(content) {}
  RenderTapeContentView(const std::vector<std::vector<std::byte>>& content) noexcept
      : vectors_(content) {}
  RenderTapeContentView(std::span<const RenderTapeCpuShadowContent> content) noexcept
      : shadows_(content), shadowBacked_(true) {}
  RenderTapeContentView(const std::vector<RenderTapeCpuShadowContent>& content) noexcept
      : shadows_(content), shadowBacked_(true) {}
  std::size_t size() const noexcept { return shadowBacked_ ? shadows_.size() : vectors_.size(); }
  bool empty() const noexcept { return size() == 0u; }
  const void* data() const noexcept { return shadowBacked_ ? static_cast<const void*>(shadows_.data())
                                                        : static_cast<const void*>(vectors_.data()); }
  const std::vector<std::byte>* vectorData() const noexcept {
    return shadowBacked_ ? nullptr : vectors_.data();
  }
  bool vectorBacked() const noexcept { return !shadowBacked_; }
  RenderTapeByteView operator[](std::size_t index) const noexcept {
    return shadowBacked_ ? shadows_[index].readBytes()
                        : RenderTapeByteView(std::span<const std::byte>(vectors_[index]));
  }
  struct Iterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = RenderTapeByteView;
    using reference = value_type;
    using pointer = void;
    using difference_type = std::ptrdiff_t;
    const RenderTapeContentView* view = nullptr;
    std::size_t index = 0u;
    value_type operator*() const noexcept { return (*view)[index]; }
    Iterator& operator++() noexcept { ++index; return *this; }
    Iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
    friend bool operator==(const Iterator&, const Iterator&) = default;
    friend difference_type operator-(const Iterator& a, const Iterator& b) noexcept {
      return static_cast<difference_type>(a.index) - static_cast<difference_type>(b.index);
    }
  };
  Iterator begin() const noexcept { return {.view = this}; }
  Iterator end() const noexcept { return {.view = this, .index = size()}; }
private:
  std::span<const std::vector<std::byte>> vectors_{};
  std::span<const RenderTapeCpuShadowContent> shadows_{};
  bool shadowBacked_ = false;
};

// Preprovisioned capture-boundary observer. Refcounts identify sharing only;
// exact distinct payload storage is joined by backing identity, never inferred
// from summed logical sizes. Overflow keeps lower bounds explicitly incomplete.
struct RenderTapePayloadLedger {
  static constexpr std::size_t kMaxBackings = 2048u;
  struct Backing {
    const void* data = nullptr;
    std::uint64_t bytes = 0u;
    std::uint64_t capacity = 0u;
    bool shared = false;
  };
  std::array<Backing, kMaxBackings> backings{};
  std::size_t backingCount = 0u;
  std::uint64_t logicalBytes = 0u;
  std::uint64_t logicalCapacity = 0u;
  std::uint64_t logicalOwners = 0u;
  std::uint64_t uniqueBytes = 0u;
  std::uint64_t uniqueCapacity = 0u;
  std::uint64_t uniqueSharedBytes = 0u;
  std::uint64_t uniqueSharedCapacity = 0u;
  bool uniqueComplete = true;
  bool totalsValid = true;

  void reset() noexcept {
    backingCount = 0u;
    logicalBytes = logicalCapacity = logicalOwners = 0u;
    uniqueBytes = uniqueCapacity = uniqueSharedBytes = uniqueSharedCapacity = 0u;
    uniqueComplete = totalsValid = true;
  }
  void add(std::uint64_t& total, std::uint64_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
      total = std::numeric_limits<std::uint64_t>::max();
      totalsValid = false;
    } else total += bytes;
  }
  void observe(const void* data, std::uint64_t bytes, std::uint64_t capacity,
               bool shared) noexcept {
    add(logicalBytes, bytes);
    add(logicalCapacity, capacity);
    if (capacity == 0u && bytes == 0u) return;
    ++logicalOwners;
    for (std::size_t i = 0u; i < backingCount; ++i) {
      auto& backing = backings[i];
      if (backing.data != data) continue;
      if (backing.bytes != bytes || backing.capacity != capacity)
        uniqueComplete = false;
      if (shared && !backing.shared) {
        backing.shared = true;
        add(uniqueSharedBytes, backing.bytes);
        add(uniqueSharedCapacity, backing.capacity);
      }
      return;
    }
    if (!data || backingCount == backings.size()) {
      uniqueComplete = false;
      return;
    }
    backings[backingCount++] = {.data = data, .bytes = bytes, .capacity = capacity, .shared = shared};
    add(uniqueBytes, bytes);
    add(uniqueCapacity, capacity);
    if (shared) {
      add(uniqueSharedBytes, bytes);
      add(uniqueSharedCapacity, capacity);
    }
  }
};

} // namespace dxmt9::d3d9
