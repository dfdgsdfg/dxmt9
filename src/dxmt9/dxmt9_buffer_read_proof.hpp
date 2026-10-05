#pragma once

#include "dxmt9/core.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace dxmt9::resources {

enum class BufferVersionObservationStatus : std::uint8_t {
  Valid, InvalidRequest, MissingBuffer, BackingUnavailable, InvalidBacking,
  InspectionFailed,
};

// Current concrete allocation identity only; deliberately no contents pointer
// or scalar contents address. This value neither retains the allocation nor
// describes a per-old-ring-entry content epoch.
struct BufferVersionBackingSnapshot {
  std::uint64_t metalHandle = 0u, byteSize = 0u;
};

// One cold, instantaneous current logical-version/backing observation. Every
// fact comes from the SAME arena inspection, including for nonversioned live
// buffers. Failure returns status only and zero facts; shadow-only or otherwise
// unmapped storage cannot qualify as physical backing evidence.
//
// The producer must independently establish successful mutation -> observation
// -> source/context attachment ordering while retaining the actual resource.
// This is not a pin, future byte proof, old-backing epoch, or authentication of
// an arbitrary caller-owned CPU content epoch by scalar revision equality.
struct BufferVersionObservation {
  BufferVersionObservationStatus status = BufferVersionObservationStatus::InvalidRequest;
  core::Handle buffer{};
  std::uint64_t byteSize = 0u, contentRevision = 0u;
  bool hasVersionedBacking = false;
  BufferVersionBackingSnapshot currentBacking{};
  bool observed() const noexcept {
    return status == BufferVersionObservationStatus::Valid;
  }
};

struct BufferReadProofRange {
  std::uint64_t byteOffset = 0u, byteSize = 0u;
};

struct BufferReadProofLimits {
  std::size_t maxRanges = 512u;
  std::uint64_t maxReadBytes = 256u * 1024u * 1024u;
};

// Borrowed inputs for one cold arena-locked read. The caller retains the actual
// source/backing lifetime and supplies its genuine sequence and completed
// watermark; these scalar values alone do not certify source ownership.
// expectedBytes is the immutable COMPLETE logical buffer version, while ranges
// describe only this consumption's fetched footprint. Adjacent/overlapping
// ranges are legal and count separately toward the bounded comparison bytes.
// This API proves physical byte equality only, not snapshot provenance or the
// authenticity of contentRevision; the source/context layer checks those
// independently before consumption.
struct BufferReadProofRequest {
  core::Handle buffer{};
  std::uint64_t sourceSequence = 0u, completedSequence = 0u;
  std::optional<core::DrawBufferBindingSnapshot> snapshot{};
  std::span<const std::uint8_t> expectedBytes{};
  std::span<const BufferReadProofRange> ranges{};
  BufferReadProofLimits limits{};
};

enum class BufferReadProofStatus : std::uint8_t {
  Valid, InvalidRequest, MissingBuffer, BackingRouteMismatch, InvalidSnapshot,
  BackingUnavailable, UnretainedBacking, InvalidExpectedExtent, InvalidRange,
  ArithmeticOverflow, CapacityExceeded, ByteMismatch, InspectionFailed,
};

// Observation only: no backing pointer escapes, no cache survives this call,
// and success grants no queue submission/completion or GPU authority. This is
// instantaneous per-buffer equality, not an atomic cross-buffer draw oracle
// or a promise that future writes preserve the footprint. Unsynchronized raw
// mapped writes and GPU writers are explicitly outside the arena-lock proof.
struct BufferReadProofResult {
  BufferReadProofStatus status = BufferReadProofStatus::InvalidRequest;
  std::uint32_t failedRange = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t provedRanges = 0u;
  std::uint64_t provedBytes = 0u;
  bool proved() const noexcept { return status == BufferReadProofStatus::Valid; }
};

inline constexpr std::size_t kBufferReadProofBatchMaxRequests = 64u;
inline constexpr std::size_t kBufferReadProofBatchMaxRanges = 65536u;

struct BufferReadProofBatchLimits {
  std::size_t maxRequests = kBufferReadProofBatchMaxRequests;
  std::size_t maxRanges = 512u;
  std::uint64_t maxReadBytes = 256u * 1024u * 1024u;
};

// Every request must name the same sourceSequence/completedSequence pair.
// Duplicate buffers, ranges and captured snapshots are legal and count
// separately. Aggregate limits are checked before byte comparisons; the
// single-request limits still apply independently to each member.
//
// Success is ONE coherent instantaneous observation of all supplied index/
// vertex ranges under the buffer arena's shared lock. It grants no source pin,
// revision authentication, old-ring epoch, GPU authority or preservation after
// unlock. Unsynchronized mapped writes and GPU writers remain excluded.
struct BufferReadProofBatchResult {
  BufferReadProofStatus status = BufferReadProofStatus::InvalidRequest;
  std::uint32_t failedRequest = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t failedRange = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t provedRequests = 0u, provedRanges = 0u;
  std::uint64_t provedBytes = 0u;
  bool proved() const noexcept { return status == BufferReadProofStatus::Valid; }
};

} // namespace dxmt9::resources
