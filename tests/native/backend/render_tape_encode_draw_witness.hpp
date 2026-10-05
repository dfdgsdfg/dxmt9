#pragma once

// Cold numeric receipts only. Callback spans are consumed synchronously; no
// source/destination pointer, byte copy, queue ticket or GPU authority escapes.
// Digests describe observed upload input, not collision-free semantic equality.
// A matched destination means immediate slice equality, never preservation
// until execution. Final native stream bindings do not prove bytes or shader
// consumption. Observation IDs name repeated native-handle values within one
// collector, not cross-run identities, content versions or retained lifetimes.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dxmt9::tests::encode_draw_witness {

enum class BufferKind : std::uint8_t {
  TransientVertexUpload, TransientIndexUpload, StagedVertexUpload,
  StagedIndexUpload, StagedVertexReuse, StagedIndexReuse,
};

enum class Failure : std::uint8_t {
  None, InvalidLimits, Suppressed, InvalidLocation, MissingDraw,
  CapacityExceeded, ArithmeticOverflow, InvalidSlice, InvalidUpload,
  DestinationMismatch, MissingReuseUpload, AllocationFailed,
};

inline const char* failureName(Failure failure) noexcept {
  switch (failure) {
  case Failure::None: return "none";
  case Failure::InvalidLimits: return "invalid-limits";
  case Failure::Suppressed: return "suppressed-metal-or-side-effects";
  case Failure::InvalidLocation: return "invalid-draw-location";
  case Failure::MissingDraw: return "missing-draw-begin";
  case Failure::CapacityExceeded: return "witness-capacity-exceeded";
  case Failure::ArithmeticOverflow: return "witness-arithmetic-overflow";
  case Failure::InvalidSlice: return "invalid-accepted-slice";
  case Failure::InvalidUpload: return "invalid-successful-upload-input";
  case Failure::DestinationMismatch: return "accepted-destination-comparison-rejected";
  case Failure::MissingReuseUpload: return "reuse-without-prior-upload-receipt";
  case Failure::AllocationFailed: return "witness-allocation-failed";
  }
  return "unknown";
}

struct Limits {
  std::size_t maxDraws = 16384u;
  std::size_t maxUploads = 65536u;
  std::size_t maxReuses = 65536u;
  std::size_t maxIssues = 65536u;
  std::size_t maxNativeBuffers = 65536u;
  std::uint64_t maxMemberBytes = 64u * 1024u * 1024u;
  std::uint64_t maxObservedBytes = 1024u * 1024u * 1024u;
};

struct DrawKey {
  std::uint64_t sequence = 0u;
  std::uint32_t command = UINT32_MAX;
  std::uint64_t subdraw = 0u;
  friend constexpr bool operator==(DrawKey, DrawKey) = default;
};

struct Slice {
  std::uint64_t buffer = 0u, offset = 0u, size = 0u;
  friend constexpr bool operator==(Slice, Slice) = default;
};

// Value result of the genuine synchronous destination reader. Equal is valid
// only for a complete accepted slice comparison performed inside this callback.
// No readable span or native pointer is retained by the collector.
struct DestinationCheck {
  enum class State : std::uint8_t { Unavailable, Equal, Rejected };
  State state = State::Unavailable;
  std::uint8_t status = 0u;
  std::uint64_t comparedBytes = 0u;
};

struct Upload {
  DrawKey draw{};
  Slice slice{};
  BufferKind kind = BufferKind::TransientVertexUpload;
  std::uint8_t reason = 0u;
  std::uint64_t sourceHandle = 0u, inputDigest = 0u;
  std::uint64_t selectedIssues = 0u;
  bool destinationCompared = false;
  std::uint8_t destinationStatus = 0u;
  std::uint32_t bufferObservationId = 0u;
};

struct Reuse {
  DrawKey draw{};
  std::uint32_t upload = UINT32_MAX;
  BufferKind kind = BufferKind::StagedVertexReuse;
};

struct Selection {
  std::uint32_t upload = UINT32_MAX;
  std::uint8_t slot = 0u;
  std::uint64_t bindingOffset = 0u;
  std::uint64_t bindingBuffer = 0u;
  std::uint32_t bufferObservationId = 0u;
};

struct Issue {
  DrawKey draw{};
  bool indexed = false;
  std::uint32_t primitiveType = 0u, instanceCount = 0u;
  std::uint64_t count = 0u, vertexStart = 0u;
  std::int32_t baseVertex = 0;
  std::uint32_t baseInstance = 0u;
  std::uint8_t indexElementBytes = 0u;
  Slice indexSlice{};
  std::uint32_t indexUpload = UINT32_MAX;
  std::uint32_t indexBufferObservationId = 0u;
  std::array<Selection, 16u> streams{};
  std::uint8_t boundStreamCount = 0u;
  // Number of bound streams with an upload receipt, not total native binds.
  std::uint8_t selectedStreamCount = 0u;
};

class Collector {
public:
  explicit Collector(Limits limits = {}) noexcept : limits_(limits) {
    if (!limits.maxDraws || limits.maxDraws > 65536u || !limits.maxUploads ||
        limits.maxUploads > 65536u || !limits.maxReuses || limits.maxReuses > 65536u ||
        !limits.maxIssues || limits.maxIssues > 65536u || !limits.maxNativeBuffers ||
        limits.maxNativeBuffers > 65536u || !limits.maxMemberBytes ||
        !limits.maxObservedBytes || limits.maxMemberBytes > 64u * 1024u * 1024u ||
        limits.maxObservedBytes > 1024u * 1024u * 1024u ||
        limits.maxMemberBytes > limits.maxObservedBytes ||
        limits.maxObservedBytes > std::numeric_limits<std::size_t>::max()) {
      reject(Failure::InvalidLimits);
      return;
    }
    try {
      draws_.reserve(limits.maxDraws);
      uploads_.reserve(limits.maxUploads);
      reuses_.reserve(limits.maxReuses);
      issues_.reserve(limits.maxIssues);
      nativeBuffers_.reserve(limits.maxNativeBuffers);
    } catch (...) { reject(Failure::AllocationFailed); }
  }

  void suppression(bool metal, bool sideEffects, bool baseLookup) noexcept {
    if (metal || sideEffects || baseLookup) reject(Failure::Suppressed);
  }

  void beginDraw(DrawKey draw) noexcept {
    if (failed()) return;
    if (!draw.sequence || draw.command == UINT32_MAX) {
      reject(Failure::InvalidLocation);
      return;
    }
    if (draws_.size() >= limits_.maxDraws) { reject(Failure::CapacityExceeded); return; }
    try { draws_.push_back(draw); current_ = draw; activeDraw_ = true; }
    catch (...) { reject(Failure::AllocationFailed); }
  }

  void beginSourceFragment(std::uint64_t sequence) noexcept {
    if (failed()) return;
    if (!sequence || fragmentBegins_ >= 65536u) {
      reject(sequence ? Failure::CapacityExceeded : Failure::InvalidLocation); return;
    }
    ++fragmentBegins_;
    fragmentSequence_ = sequence;
    activeDraw_ = false;
  }

  // New render encoders have no inherited bindings. A source fragment may
  // continue the same encoder, so its preamble/epilogue must NOT call this.
  void renderPassBoundary() noexcept {
    bindings_ = {};
    activeDraw_ = false;
  }

  void endSourceFragment(std::uint64_t sequence) noexcept {
    if (failed()) return;
    if (sequence != fragmentSequence_ || fragmentEnds_ >= fragmentBegins_) {
      reject(Failure::InvalidLocation); return;
    }
    ++fragmentEnds_;
    activeDraw_ = false;
  }

  void observeSlice(BufferKind kind, std::uint8_t reason,
      std::uint64_t sourceHandle, std::span<const std::uint8_t> input,
      Slice slice, DestinationCheck destination = {}) noexcept {
    if (failed()) return;
    if (!activeDraw_) { reject(Failure::MissingDraw); return; }
    if (static_cast<unsigned>(kind) > static_cast<unsigned>(BufferKind::StagedIndexReuse)) {
      reject(Failure::InvalidUpload); return;
    }
    if (!slice.buffer || !slice.size) { reject(Failure::InvalidSlice); return; }
    if (slice.offset > UINT64_MAX - slice.size) {
      reject(Failure::ArithmeticOverflow); return;
    }
    const bool reuse = kind == BufferKind::StagedVertexReuse || kind == BufferKind::StagedIndexReuse;
    if (reuse) {
      if (!input.empty() || !sourceHandle) { reject(Failure::InvalidUpload); return; }
      const auto expectedKind = kind == BufferKind::StagedVertexReuse
          ? BufferKind::StagedVertexUpload : BufferKind::StagedIndexUpload;
      const auto found = findUpload(slice, expectedKind, sourceHandle, true);
      if (found == UINT32_MAX) { reject(Failure::MissingReuseUpload); return; }
      if (reuses_.size() >= limits_.maxReuses) { reject(Failure::CapacityExceeded); return; }
      try { reuses_.push_back({current_, found, kind}); }
      catch (...) { reject(Failure::AllocationFailed); }
      return;
    }
    if (input.empty() || input.size() != slice.size ||
        ((kind == BufferKind::StagedVertexUpload || kind == BufferKind::StagedIndexUpload) !=
         (sourceHandle != 0u))) { reject(Failure::InvalidUpload); return; }
    if (slice.size > limits_.maxMemberBytes || uploads_.size() >= limits_.maxUploads) {
      reject(Failure::CapacityExceeded); return;
    }
    const bool compare = destination.state == DestinationCheck::State::Equal;
    if (destination.state == DestinationCheck::State::Rejected ||
        (compare && destination.comparedBytes != slice.size)) {
      destinationFailureStatus_ = destination.status;
      reject(Failure::DestinationMismatch); return;
    }
    // Fingerprint reads the input once; memcmp reads input and destination.
    if (compare && slice.size > UINT64_MAX / 3u) { reject(Failure::ArithmeticOverflow); return; }
    const auto cost = compare ? slice.size * 3u : slice.size;
    if (observedBytes_ > UINT64_MAX - cost) { reject(Failure::ArithmeticOverflow); return; }
    if (cost > limits_.maxObservedBytes - observedBytes_) { reject(Failure::CapacityExceeded); return; }
    const auto bufferId = observeNativeBuffer(slice.buffer);
    if (failed()) return;
    observedBytes_ += cost;
    std::uint64_t digest = 14695981039346656037ull;
    for (const auto byte : input) { digest ^= byte; digest *= 1099511628211ull; }
    try {
      uploads_.push_back({current_, slice, kind, reason, sourceHandle, digest,
                          0u, compare, destination.status, bufferId});
    } catch (...) { reject(Failure::AllocationFailed); }
  }

  void bindVertex(std::uint64_t buffer, std::uint64_t offset, std::uint8_t slot) noexcept {
    if (failed()) return;
    const auto stream = streamForSlot(slot);
    if (stream == UINT32_MAX) return;
    const auto bufferId = observeNativeBuffer(buffer);
    if (!failed()) bindings_[stream] = {buffer, offset, bufferId};
  }

  void bindVertexBytes(std::uint8_t slot) noexcept {
    if (failed()) return;
    const auto stream = streamForSlot(slot);
    if (stream != UINT32_MAX) bindings_[stream] = {};
  }

  void issue(bool indexed, std::uint32_t primitiveType, std::uint64_t count,
      std::uint64_t vertexStart, std::uint32_t instances, std::int32_t baseVertex,
      std::uint32_t baseInstance, Slice index = {}, std::uint8_t elementBytes = 0u) noexcept {
    if (failed()) return;
    if (!activeDraw_) { reject(Failure::MissingDraw); return; }
    if (issues_.size() >= limits_.maxIssues) { reject(Failure::CapacityExceeded); return; }
    Issue row{.draw = current_, .indexed = indexed, .primitiveType = primitiveType,
              .instanceCount = instances, .count = count, .vertexStart = vertexStart,
              .baseVertex = baseVertex, .baseInstance = baseInstance,
              .indexElementBytes = elementBytes, .indexSlice = index};
    if (indexed) {
      if ((elementBytes != 2u && elementBytes != 4u) || count > UINT64_MAX / elementBytes) {
        reject(Failure::ArithmeticOverflow); return;
      }
      row.indexSlice.size = count * elementBytes;
      if (!index.buffer || index.offset > UINT64_MAX - row.indexSlice.size) {
        reject(Failure::InvalidSlice); return;
      }
      row.indexUpload = findSelected(index.buffer, index.offset, row.indexSlice.size, false);
      row.indexBufferObservationId = observeNativeBuffer(index.buffer);
      if (failed()) return;
    }
    for (std::size_t stream = 0u; stream < bindings_.size(); ++stream) {
      const auto binding = bindings_[stream];
      if (!binding.buffer) continue;
      const auto upload = findSelected(binding.buffer, binding.offset, 1u, true);
      row.streams[row.boundStreamCount++] = {upload,
          static_cast<std::uint8_t>(stream ? stream + 5u : 1u), binding.offset,
          binding.buffer, binding.bufferObservationId};
      if (upload != UINT32_MAX) ++row.selectedStreamCount;
    }
    try {
      issues_.push_back(row);
      for (std::size_t i = 0u; i < row.boundStreamCount; ++i)
        if (row.streams[i].upload != UINT32_MAX)
          ++uploads_[row.streams[i].upload].selectedIssues;
      if (row.indexUpload != UINT32_MAX) ++uploads_[row.indexUpload].selectedIssues;
    } catch (...) { reject(Failure::AllocationFailed); }
  }

  bool failed() const noexcept { return failure_ != Failure::None; }
  Failure failure() const noexcept { return failure_; }
  const Limits& limits() const noexcept { return limits_; }
  std::uint64_t observedBytes() const noexcept { return observedBytes_; }
  std::uint64_t fragmentBegins() const noexcept { return fragmentBegins_; }
  std::uint64_t fragmentEnds() const noexcept { return fragmentEnds_; }
  std::uint8_t destinationFailureStatus() const noexcept { return destinationFailureStatus_; }
  std::uint64_t destinationBudget() const noexcept {
    if (failed() || uploads_.size() >= limits_.maxUploads) return 0u;
    return std::min(limits_.maxMemberBytes, (limits_.maxObservedBytes - observedBytes_) / 3u);
  }
  std::span<const DrawKey> draws() const noexcept { return draws_; }
  std::span<const Upload> uploads() const noexcept { return uploads_; }
  std::span<const Reuse> reuses() const noexcept { return reuses_; }
  std::span<const Issue> issues() const noexcept { return issues_; }
  std::size_t nativeBufferCount() const noexcept { return nativeBuffers_.size(); }
  std::uint32_t bufferObservationId(std::uint64_t buffer) const noexcept {
    if (!buffer) return 0u;
    const auto found = std::find(nativeBuffers_.begin(), nativeBuffers_.end(), buffer);
    return found == nativeBuffers_.end() ? 0u
        : static_cast<std::uint32_t>(found - nativeBuffers_.begin() + 1u);
  }
  std::size_t orphanUploads() const noexcept {
    std::size_t count = 0u;
    for (const auto& upload : uploads_) if (!upload.selectedIssues) ++count;
    return count;
  }

private:
  struct Binding {
    std::uint64_t buffer = 0u, offset = 0u;
    std::uint32_t bufferObservationId = 0u;
  };
  void reject(Failure failure) noexcept { if (!failed()) failure_ = failure; }
  std::uint32_t observeNativeBuffer(std::uint64_t buffer) noexcept {
    if (!buffer) return 0u;
    if (const auto id = bufferObservationId(buffer)) return id;
    if (nativeBuffers_.size() >= limits_.maxNativeBuffers) {
      reject(Failure::CapacityExceeded); return 0u;
    }
    try {
      nativeBuffers_.push_back(buffer);
      return static_cast<std::uint32_t>(nativeBuffers_.size());
    } catch (...) { reject(Failure::AllocationFailed); return 0u; }
  }
  static std::uint32_t streamForSlot(std::uint8_t slot) noexcept {
    return slot == 1u ? 0u : slot >= 6u && slot <= 20u ? slot - 5u : UINT32_MAX;
  }
  std::uint32_t findUpload(Slice slice, BufferKind kind, std::uint64_t source,
      bool exact) const noexcept {
    for (std::size_t i = uploads_.size(); i > 0u; --i) {
      const auto& row = uploads_[i - 1u];
      if (row.kind == kind && row.sourceHandle == source &&
          (!exact || row.slice == slice)) return static_cast<std::uint32_t>(i - 1u);
    }
    return UINT32_MAX;
  }
  std::uint32_t findSelected(std::uint64_t buffer, std::uint64_t offset,
      std::uint64_t bytes, bool vertex) const noexcept {
    if (!buffer || !bytes) return UINT32_MAX;
    for (std::size_t i = uploads_.size(); i > 0u; --i) {
      const auto& row = uploads_[i - 1u];
      const bool rowVertex = row.kind == BufferKind::TransientVertexUpload ||
                             row.kind == BufferKind::StagedVertexUpload;
      if (rowVertex == vertex && row.slice.buffer == buffer && offset >= row.slice.offset &&
          offset - row.slice.offset < row.slice.size && bytes <= row.slice.size - (offset - row.slice.offset))
        return static_cast<std::uint32_t>(i - 1u);
    }
    return UINT32_MAX;
  }
  Limits limits_{};
  Failure failure_ = Failure::None;
  DrawKey current_{};
  bool activeDraw_ = false;
  std::uint64_t observedBytes_ = 0u;
  std::uint64_t fragmentSequence_ = 0u, fragmentBegins_ = 0u, fragmentEnds_ = 0u;
  std::uint8_t destinationFailureStatus_ = 0u;
  std::array<Binding, 16u> bindings_{};
  std::vector<DrawKey> draws_{};
  std::vector<Upload> uploads_{};
  std::vector<Reuse> reuses_{};
  std::vector<Issue> issues_{};
  std::vector<std::uint64_t> nativeBuffers_{};
};

} // namespace dxmt9::tests::encode_draw_witness
