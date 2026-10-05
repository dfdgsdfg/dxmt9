#pragma once

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxmt9::test {

struct CpuTapeResourceReceipt {
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
  d3d9::RenderTapeDigest digest{};
  std::uint64_t revision = 0u;
};

struct CpuTapeDepthStencilObservation {
  std::uint32_t disposition = 0u, format = 0u, width = 0u, height = 0u;
  std::uint32_t depthPitch = 0u, stencilPitch = 0u;
  std::uint64_t stencilOffset = 0u, byteCount = 0u;
};

struct CpuTapeResourceObservationInput {
  // identity is the bound object; contentIdentity is its qualified seed owner.
  // A surviving surface alias keeps its own identity and its original parent.
  D9CWireObjectIdentity identity{}, contentIdentity{};
  std::optional<std::uint32_t> aliasSubresource{};
  std::optional<CpuTapeDepthStencilObservation> depthStencil{};
  std::span<const CpuTapeResourceReceipt> receipts{};
};

struct CpuTapeResourceObservationLimits {
  // The serial executor supplies its admitted plan's mutation count. Standalone
  // callers default to the existing General grammar's 1024-mutation budget.
  std::size_t maxInputReceipts = 1024u;
};

enum class CpuTapeResourceObservationStatus : std::uint8_t {
  Accepted, InvalidIdentity, InvalidMetadata, InvalidReceipt,
  CapacityExceeded, AllocationFailed,
};

class CpuTapeResourceObservation;
CpuTapeResourceObservationStatus buildCpuTapeResourceObservation(
    const CpuTapeResourceObservationInput&, CpuTapeResourceObservation&,
    CpuTapeResourceObservationLimits = {}) noexcept;
bool appendCpuTapeResourceObservation(
    const CpuTapeResourceObservation&, std::uint64_t&) noexcept;

// Cold value-owned normalization only. This is not a record/subdraw epoch
// binding, a retained resource/backing owner, or a concurrent consumer oracle.
// Only the producer qualifies wrappers, alias parents and retirement facts.
class CpuTapeResourceObservation {
public:
  CpuTapeResourceObservation() = default;
  CpuTapeResourceObservation(const CpuTapeResourceObservation&) = default;
  CpuTapeResourceObservation& operator=(const CpuTapeResourceObservation& other) {
    if (this != &other) {
      CpuTapeResourceObservation candidate(other);
      *this = std::move(candidate);
    }
    return *this;
  }
  CpuTapeResourceObservation(CpuTapeResourceObservation&& other) noexcept
      : identity_(other.identity_), contentIdentity_(other.contentIdentity_),
        aliasSubresource_(other.aliasSubresource_), depthStencil_(other.depthStencil_),
        receipts_(std::move(other.receipts_)), valid_(std::exchange(other.valid_, false)) {}
  CpuTapeResourceObservation& operator=(CpuTapeResourceObservation&& other) noexcept {
    if (this != &other) {
      identity_ = other.identity_;
      contentIdentity_ = other.contentIdentity_;
      aliasSubresource_ = other.aliasSubresource_;
      depthStencil_ = other.depthStencil_;
      receipts_ = std::move(other.receipts_);
      valid_ = std::exchange(other.valid_, false);
    }
    return *this;
  }

  bool valid() const noexcept { return valid_; }
  const D9CWireObjectIdentity& identity() const noexcept { return identity_; }
  const D9CWireObjectIdentity& contentIdentity() const noexcept { return contentIdentity_; }
  std::optional<std::uint32_t> aliasSubresource() const noexcept { return aliasSubresource_; }
  const std::optional<CpuTapeDepthStencilObservation>& depthStencil() const noexcept {
    return depthStencil_;
  }
  std::span<const CpuTapeResourceReceipt> receipts() const noexcept { return receipts_; }

private:
  friend CpuTapeResourceObservationStatus buildCpuTapeResourceObservation(
      const CpuTapeResourceObservationInput&, CpuTapeResourceObservation&,
      CpuTapeResourceObservationLimits) noexcept;
  friend bool appendCpuTapeResourceObservation(
      const CpuTapeResourceObservation&, std::uint64_t&) noexcept;
  D9CWireObjectIdentity identity_{}, contentIdentity_{};
  std::optional<std::uint32_t> aliasSubresource_{};
  std::optional<CpuTapeDepthStencilObservation> depthStencil_{};
  std::vector<CpuTapeResourceReceipt> receipts_{};
  bool valid_ = false;
};

namespace cpu_resource_observation_detail {

inline bool sameIdentity(const D9CWireObjectIdentity& a,
                         const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation && a.objectId == b.objectId;
}

inline bool validIdentity(const D9CWireObjectIdentity& identity) noexcept {
  return identity.kind <= D9C_CHUNK_HANDLE_KIND_BUFFER &&
      identity.generation != 0u && identity.objectId != 0u;
}

inline void appendBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::byte*>(data);
  for (std::size_t index = 0u; index < size; ++index) {
    hash ^= std::to_integer<std::uint8_t>(bytes[index]);
    hash *= 1099511628211ull;
  }
}

template <class T>
inline void appendScalar(std::uint64_t& hash, T value) noexcept {
  static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
  appendBytes(hash, &value, sizeof(value));
}

} // namespace cpu_resource_observation_detail

inline CpuTapeResourceObservationStatus buildCpuTapeResourceObservation(
    const CpuTapeResourceObservationInput& input, CpuTapeResourceObservation& out,
    CpuTapeResourceObservationLimits limits) noexcept {
  using Status = CpuTapeResourceObservationStatus;
  namespace detail = cpu_resource_observation_detail;
  // Match General admission's index budget; zero admits an empty receipt set.
  if (limits.maxInputReceipts > std::numeric_limits<std::uint32_t>::max() - 1u ||
      input.receipts.size() > limits.maxInputReceipts) return Status::CapacityExceeded;
  if (!detail::validIdentity(input.identity) || !detail::validIdentity(input.contentIdentity) ||
      (input.aliasSubresource
          ? input.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
              input.contentIdentity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE
          : !detail::sameIdentity(input.identity, input.contentIdentity)))
    return Status::InvalidIdentity;
  if (input.depthStencil) {
    const auto& depth = *input.depthStencil;
    const D9CSurfaceDesc surface{.format = depth.format, .resourceType = 1u,
        .usage = 2u, .width = depth.width, .height = depth.height, .depth = 1u};
    d3d9::RenderTapeD24S8SnapshotLayout layout{};
    if (input.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE || input.aliasSubresource ||
        depth.disposition != static_cast<std::uint32_t>(
            d3d9::RenderTapeInitialContentDisposition::CompleteDepthStencilFloat32UInt8V1) ||
        !d3d9::renderTapeDeriveD24S8SnapshotLayout(surface, layout) ||
        depth.depthPitch != layout.depthPitch || depth.stencilPitch != layout.stencilPitch ||
        depth.stencilOffset != layout.stencilOffset || depth.byteCount != layout.byteCount)
      return Status::InvalidMetadata;
  }
  const auto selected = [&](const CpuTapeResourceReceipt& receipt) noexcept {
    return detail::sameIdentity(receipt.identity, input.contentIdentity) &&
        (!input.aliasSubresource || receipt.subresource == *input.aliasSubresource);
  };
  std::size_t count = 0u;
  for (const auto& receipt : input.receipts) {
    if (!detail::validIdentity(receipt.identity) || !receipt.revision)
      return Status::InvalidReceipt;
    if (selected(receipt)) ++count;
  }
  try {
    CpuTapeResourceObservation candidate;
    if (count > candidate.receipts_.max_size()) return Status::CapacityExceeded;
    candidate.identity_ = input.identity;
    candidate.contentIdentity_ = input.contentIdentity;
    candidate.aliasSubresource_ = input.aliasSubresource;
    candidate.depthStencil_ = input.depthStencil;
    candidate.receipts_.reserve(count);
    for (const auto& receipt : input.receipts)
      if (selected(receipt)) candidate.receipts_.push_back(receipt);
    candidate.valid_ = true;
    out = std::move(candidate);
    return Status::Accepted;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

inline bool appendCpuTapeResourceObservation(
    const CpuTapeResourceObservation& observation, std::uint64_t& hash) noexcept {
  if (!observation.valid_) return false;
  namespace detail = cpu_resource_observation_detail;
  auto candidate = hash;
  detail::appendScalar(candidate, observation.identity_.kind);
  detail::appendScalar(candidate, observation.identity_.generation);
  detail::appendScalar(candidate, observation.identity_.objectId);
  if (observation.depthStencil_) {
    const auto& depth = *observation.depthStencil_;
    detail::appendScalar(candidate, depth.disposition);
    detail::appendScalar(candidate, depth.format);
    detail::appendScalar(candidate, depth.width);
    detail::appendScalar(candidate, depth.height);
    detail::appendScalar(candidate, depth.depthPitch);
    detail::appendScalar(candidate, depth.stencilPitch);
    detail::appendScalar(candidate, depth.stencilOffset);
    detail::appendScalar(candidate, depth.byteCount);
  }
  for (const auto& receipt : observation.receipts_) {
    detail::appendScalar(candidate, receipt.subresource);
    detail::appendBytes(candidate, receipt.digest.data(), receipt.digest.size());
    detail::appendScalar(candidate, receipt.revision);
  }
  // No presence bits, parent identity, native handles, or struct padding enter
  // the legacy stream. Even the empty set terminates in this uint32 count.
  detail::appendScalar(candidate, static_cast<std::uint32_t>(observation.receipts_.size()));
  hash = candidate;
  return true;
}

} // namespace dxmt9::test
