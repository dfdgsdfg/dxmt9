#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dxmt9::d3d9 {

class RenderTapeCpuShadowContent;

// Capture-only, owned, immutable version. The allocation itself is mutable
// storage, but only its durable-shadow friend may reuse it after every lease
// has gone away. Public readers never obtain a mutable pointer/reference.
class RenderTapeImmutablePayload {
  struct Storage {
    explicit Storage(std::vector<std::byte>&& input) noexcept : bytes(std::move(input)) {}
    std::vector<std::byte> bytes;
  };
  friend class RenderTapeCpuShadowContent;
public:
  RenderTapeImmutablePayload() noexcept = default;
  RenderTapeImmutablePayload(std::vector<std::byte>&& bytes)
      : owner_(std::make_shared<Storage>(std::move(bytes))) {}
  RenderTapeImmutablePayload(const std::vector<std::byte>& bytes)
      : RenderTapeImmutablePayload(std::vector<std::byte>(bytes)) {}

  std::span<const std::byte> readBytes() const noexcept {
    return owner_ ? std::span<const std::byte>(owner_->bytes) : std::span<const std::byte>{};
  }
  const std::byte* data() const noexcept { return readBytes().data(); }
  std::size_t size() const noexcept { return readBytes().size(); }
  bool empty() const noexcept { return size() == 0u; }
  auto begin() const noexcept { return readBytes().begin(); }
  auto end() const noexcept { return readBytes().end(); }
  std::size_t capacity() const noexcept { return owner_ ? owner_->bytes.capacity() : 0u; }
  bool hasBacking() const noexcept { return static_cast<bool>(owner_); }
  bool shared() const noexcept { return owner_ && owner_.use_count() > 1; }

  friend bool operator==(const RenderTapeImmutablePayload& a,
                         const RenderTapeImmutablePayload& b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }
  friend bool operator==(const RenderTapeImmutablePayload& a,
                         const std::vector<std::byte>& b) noexcept {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
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
};

// Exactly one active representation: a private writable vector, or a frozen
// owner with no private payload. Freeze changes ownership, never bytes or
// completeness. A failed writer leaves the complete committed version intact.
class RenderTapeCpuShadowContent {
public:
  RenderTapeCpuShadowContent() noexcept = default;
  explicit RenderTapeCpuShadowContent(std::vector<std::byte>&& bytes) noexcept
      : private_(std::move(bytes)) {}
  explicit RenderTapeCpuShadowContent(RenderTapeImmutablePayload payload) noexcept
      : frozen_(std::move(payload)) {}
  RenderTapeCpuShadowContent(const RenderTapeCpuShadowContent&) = delete;
  RenderTapeCpuShadowContent& operator=(const RenderTapeCpuShadowContent&) = delete;
  RenderTapeCpuShadowContent(RenderTapeCpuShadowContent&&) noexcept = default;
  RenderTapeCpuShadowContent& operator=(RenderTapeCpuShadowContent&&) noexcept = default;

  std::span<const std::byte> readBytes() const noexcept {
    return frozen_.hasBacking() ? frozen_.readBytes() : std::span<const std::byte>(private_);
  }
  std::size_t size() const noexcept { return readBytes().size(); }
  bool empty() const noexcept { return readBytes().empty(); }
  std::size_t capacity() const noexcept {
    return frozen_.hasBacking() ? frozen_.capacity() : private_.capacity();
  }
  bool shared() const noexcept { return frozen_.shared(); }

  RenderTapeImmutablePayload freeze() {
    if (writing_)
      throw std::logic_error("render-tape CPU mutation is active");
    if (empty())
      throw std::invalid_argument("cannot freeze unknown render-tape CPU content");
    if (!frozen_.hasBacking() && (!private_.empty() || private_.capacity() != 0u)) {
      // make_shared allocates before Storage's noexcept vector move. If it
      // throws, private_ still owns every original byte/capacity/extent.
      RenderTapeImmutablePayload payload(std::move(private_));
      frozen_ = std::move(payload);
    }
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
      auto* reusable = frozen_.hasBacking()
          ? (frozen_.owner_.use_count() == 1 ? &frozen_.owner_->bytes : nullptr) : &private_;
      // Stage overlapping input first: explicit callers may pass a borrowed
      // read view of this very version, not only external lock bytes.
      std::vector<std::byte> incoming;
      if (reusable && !bytes.empty() && !reusable->empty()) {
        const auto before = std::less<const std::byte*>{};
        if (before(bytes.data(), reusable->data() + reusable->size()) &&
            before(reusable->data(), bytes.data() + bytes.size())) {
          incoming.assign(bytes.begin(), bytes.end());
          bytes = incoming;
        }
      }
      const bool inPlace = reusable &&
          (!plan.fullOverwrite || reusable->capacity() >= bytes.size());
      std::vector<std::byte> replacement;
      auto* destination = inPlace ? reusable : &replacement;
      if (plan.fullOverwrite) {
        // Full observed overwrite copies incoming bytes, never the old seed.
        destination->assign(bytes.begin(), bytes.end());
      } else {
        if (!inPlace) {
          const auto old = readBytes();
          replacement.assign(old.begin(), old.end());
        }
        for (std::uint64_t slice = 0u; slice < plan.slices; ++slice)
          for (std::uint64_t row = 0u; row < plan.rows; ++row)
            std::memcpy(destination->data() + plan.destinationOffset +
                            slice * plan.slicePitch + row * plan.rowPitch,
                        bytes.data() + slice * sourceSlice + row * plan.rowBytes,
                        static_cast<std::size_t>(plan.rowBytes));
      }
      if (!inPlace) {
        private_.swap(replacement);
        frozen_ = {};
      }
      return RenderTapeShadowWriteStatus::Accepted;
    } catch (...) {
      return RenderTapeShadowWriteStatus::AllocationFailed;
    }
  }
private:
  std::vector<std::byte> private_{};
  RenderTapeImmutablePayload frozen_{};
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
  std::span<const std::byte> operator[](std::size_t index) const noexcept {
    return shadowBacked_ ? shadows_[index].readBytes() : std::span<const std::byte>(vectors_[index]);
  }
  struct Iterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::span<const std::byte>;
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
// exact distinct payload storage is joined by backing address, never inferred
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
    if (capacity == 0u) return;
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
