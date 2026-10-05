#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace dxmt9::d3d9 {

// Cold capture admission policies, distinct from the session's eventual
// unique-blob budget. Equal shape/content never merges these reservations.
inline constexpr std::uint64_t kRenderTapeSnapshotRetainedHardLimitBytes =
    1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kRenderTapeSnapshotPerResourceHardLimitBytes =
    64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kRenderTapeSnapshotTransientHardLimitBytes =
    128ull * 1024ull * 1024ull;

enum class RenderTapeSnapshotBudgetKind : std::uint8_t {
  Retained,
  Transient,
};

enum class RenderTapeSnapshotBudgetStatus : std::uint8_t {
  Complete,
  NotInitialized,
  AlreadyInitialized,
  InvalidLimits,
  InvalidState,
  InvalidRequest,
  ZeroBytes,
  Overflow,
  PerResourceLimit,
  RetainedLimit,
  TransientLimit,
  InvalidRelease,
  AllocationFailed,
  InternalFailure,
};

inline const char* renderTapeSnapshotBudgetStatusName(
    RenderTapeSnapshotBudgetStatus status) noexcept {
  switch (status) {
  case RenderTapeSnapshotBudgetStatus::Complete: return "complete";
  case RenderTapeSnapshotBudgetStatus::NotInitialized: return "not_initialized";
  case RenderTapeSnapshotBudgetStatus::AlreadyInitialized: return "already_initialized";
  case RenderTapeSnapshotBudgetStatus::InvalidLimits: return "invalid_limits";
  case RenderTapeSnapshotBudgetStatus::InvalidState: return "invalid_state";
  case RenderTapeSnapshotBudgetStatus::InvalidRequest: return "invalid_request";
  case RenderTapeSnapshotBudgetStatus::ZeroBytes: return "zero_bytes";
  case RenderTapeSnapshotBudgetStatus::Overflow: return "overflow";
  case RenderTapeSnapshotBudgetStatus::PerResourceLimit: return "per_resource_limit";
  case RenderTapeSnapshotBudgetStatus::RetainedLimit: return "retained_limit";
  case RenderTapeSnapshotBudgetStatus::TransientLimit: return "transient_limit";
  case RenderTapeSnapshotBudgetStatus::InvalidRelease: return "invalid_release";
  case RenderTapeSnapshotBudgetStatus::AllocationFailed: return "allocation_failed";
  case RenderTapeSnapshotBudgetStatus::InternalFailure: return "internal_failure";
  }
  return "unknown";
}

struct RenderTapeSnapshotBudgetLimits {
  std::uint64_t retainedBytes = 0u;
  std::uint64_t perResourceBytes = 0u;
  std::uint64_t transientBytes = 0u;

  bool operator==(const RenderTapeSnapshotBudgetLimits&) const = default;
};

struct RenderTapeSnapshotBudgetGauges {
  std::uint64_t retainedCurrentBytes = 0u;
  std::uint64_t retainedPeakBytes = 0u;
  std::uint64_t transientCurrentBytes = 0u;
  std::uint64_t transientPeakBytes = 0u;

  bool operator==(const RenderTapeSnapshotBudgetGauges&) const = default;
};

struct RenderTapeSnapshotBudgetState {
  RenderTapeSnapshotBudgetLimits limits{};
  RenderTapeSnapshotBudgetGauges gauges{};

  bool operator==(const RenderTapeSnapshotBudgetState&) const = default;
};

// The source is session.limits().maxBlobBytes, after session normalization.
// Reject an unnormalized input rather than silently establishing a new policy.
inline RenderTapeSnapshotBudgetLimits renderTapeDeriveSnapshotBudgetLimits(
    std::uint64_t normalizedMaxBlobBytes) noexcept {
  if (normalizedMaxBlobBytes == 0u ||
      normalizedMaxBlobBytes > kRenderTapeSnapshotRetainedHardLimitBytes)
    return {};
  return {
      normalizedMaxBlobBytes,
      std::min(normalizedMaxBlobBytes,
               kRenderTapeSnapshotPerResourceHardLimitBytes),
      std::min(normalizedMaxBlobBytes * 2u,
               kRenderTapeSnapshotTransientHardLimitBytes),
  };
}

inline bool renderTapeSnapshotBudgetLimitsValid(
    const RenderTapeSnapshotBudgetLimits& limits) noexcept {
  return limits.retainedBytes != 0u &&
      limits.retainedBytes <= kRenderTapeSnapshotRetainedHardLimitBytes &&
      limits.perResourceBytes != 0u &&
      limits.perResourceBytes <= limits.retainedBytes &&
      limits.perResourceBytes <= kRenderTapeSnapshotPerResourceHardLimitBytes &&
      limits.transientBytes != 0u &&
      limits.transientBytes <= limits.retainedBytes * 2u &&
      limits.transientBytes <= kRenderTapeSnapshotTransientHardLimitBytes;
}

inline bool renderTapeSnapshotBudgetStateValid(
    const RenderTapeSnapshotBudgetState& state) noexcept {
  return renderTapeSnapshotBudgetLimitsValid(state.limits) &&
      state.gauges.retainedCurrentBytes <= state.gauges.retainedPeakBytes &&
      state.gauges.retainedPeakBytes <= state.limits.retainedBytes &&
      state.gauges.transientCurrentBytes <= state.gauges.transientPeakBytes &&
      state.gauges.transientPeakBytes <= state.limits.transientBytes;
}

inline RenderTapeSnapshotBudgetStatus renderTapeSnapshotBudgetCheckedMultiply(
    std::uint64_t bytes, std::uint64_t copies, std::uint64_t& out) noexcept {
  out = 0u;
  if (bytes == 0u || copies == 0u)
    return RenderTapeSnapshotBudgetStatus::ZeroBytes;
  if (bytes > std::numeric_limits<std::uint64_t>::max() / copies)
    return RenderTapeSnapshotBudgetStatus::Overflow;
  out = bytes * copies;
  return RenderTapeSnapshotBudgetStatus::Complete;
}

// Pure reducer: failures leave both current and peak untouched. Retained
// charges describe a whole independent payload; transient charges describe
// the caller's additional staging allocations for the same resource.
inline RenderTapeSnapshotBudgetStatus renderTapeSnapshotBudgetAdmit(
    RenderTapeSnapshotBudgetState& state, RenderTapeSnapshotBudgetKind kind,
    std::uint64_t resourceBytes, std::uint64_t chargedBytes) noexcept {
  if (!renderTapeSnapshotBudgetLimitsValid(state.limits))
    return RenderTapeSnapshotBudgetStatus::InvalidLimits;
  if (!renderTapeSnapshotBudgetStateValid(state))
    return RenderTapeSnapshotBudgetStatus::InvalidState;
  if (kind != RenderTapeSnapshotBudgetKind::Retained &&
      kind != RenderTapeSnapshotBudgetKind::Transient)
    return RenderTapeSnapshotBudgetStatus::InvalidRequest;
  if (resourceBytes == 0u || chargedBytes == 0u)
    return RenderTapeSnapshotBudgetStatus::ZeroBytes;
  if (kind == RenderTapeSnapshotBudgetKind::Retained &&
      resourceBytes != chargedBytes)
    return RenderTapeSnapshotBudgetStatus::InvalidRequest;
  const auto current = kind == RenderTapeSnapshotBudgetKind::Retained
      ? state.gauges.retainedCurrentBytes
      : state.gauges.transientCurrentBytes;
  if (chargedBytes > std::numeric_limits<std::uint64_t>::max() - current)
    return RenderTapeSnapshotBudgetStatus::Overflow;
  if (resourceBytes > state.limits.perResourceBytes)
    return RenderTapeSnapshotBudgetStatus::PerResourceLimit;
  const auto limit = kind == RenderTapeSnapshotBudgetKind::Retained
      ? state.limits.retainedBytes : state.limits.transientBytes;
  if (chargedBytes > limit - current)
    return kind == RenderTapeSnapshotBudgetKind::Retained
        ? RenderTapeSnapshotBudgetStatus::RetainedLimit
        : RenderTapeSnapshotBudgetStatus::TransientLimit;
  auto& committedCurrent = kind == RenderTapeSnapshotBudgetKind::Retained
      ? state.gauges.retainedCurrentBytes : state.gauges.transientCurrentBytes;
  auto& committedPeak = kind == RenderTapeSnapshotBudgetKind::Retained
      ? state.gauges.retainedPeakBytes : state.gauges.transientPeakBytes;
  committedCurrent += chargedBytes;
  committedPeak = std::max(committedPeak, committedCurrent);
  return RenderTapeSnapshotBudgetStatus::Complete;
}

inline RenderTapeSnapshotBudgetStatus renderTapeSnapshotBudgetRelease(
    RenderTapeSnapshotBudgetState& state, RenderTapeSnapshotBudgetKind kind,
    std::uint64_t chargedBytes) noexcept {
  if (!renderTapeSnapshotBudgetLimitsValid(state.limits))
    return RenderTapeSnapshotBudgetStatus::InvalidLimits;
  if (!renderTapeSnapshotBudgetStateValid(state))
    return RenderTapeSnapshotBudgetStatus::InvalidState;
  if (kind != RenderTapeSnapshotBudgetKind::Retained &&
      kind != RenderTapeSnapshotBudgetKind::Transient)
    return RenderTapeSnapshotBudgetStatus::InvalidRequest;
  if (chargedBytes == 0u)
    return RenderTapeSnapshotBudgetStatus::ZeroBytes;
  auto& current = kind == RenderTapeSnapshotBudgetKind::Retained
      ? state.gauges.retainedCurrentBytes : state.gauges.transientCurrentBytes;
  if (chargedBytes > current)
    return RenderTapeSnapshotBudgetStatus::InvalidRelease;
  current -= chargedBytes;
  return RenderTapeSnapshotBudgetStatus::Complete;
}

namespace snapshot_budget_detail {

struct SharedState {
  explicit SharedState(RenderTapeSnapshotBudgetLimits limits) noexcept
      : value{limits, {}} {}
  std::mutex mutex{};
  RenderTapeSnapshotBudgetState value{};
};

struct Receipt {
  Receipt(std::shared_ptr<SharedState> state,
          RenderTapeSnapshotBudgetKind kind, std::uint64_t resourceBytes,
          std::uint64_t chargedBytes) noexcept
      : state(std::move(state)), kind(kind), resourceBytes(resourceBytes),
        chargedBytes(chargedBytes) {}
  ~Receipt() noexcept {
    if (!active) return;
    std::lock_guard lock(state->mutex);
    [[maybe_unused]] const auto status = renderTapeSnapshotBudgetRelease(
        state->value, kind, chargedBytes);
    assert(status == RenderTapeSnapshotBudgetStatus::Complete);
  }
  std::shared_ptr<SharedState> state{};
  RenderTapeSnapshotBudgetKind kind = RenderTapeSnapshotBudgetKind::Retained;
  std::uint64_t resourceBytes = 0u;
  std::uint64_t chargedBytes = 0u;
  bool active = false;
  mutable const void* boundPayload = nullptr;
  mutable bool payloadBound = false;
};

} // namespace snapshot_budget_detail

class RenderTapeSnapshotBudget;

// Sharing this receipt is an explicit ownership operation, not proof that two
// facades alias. Admission must precede payload allocation. In an owning
// payload, declare the lease before the byte vector so the bytes die first.
class RenderTapeSnapshotBudgetLease {
public:
  RenderTapeSnapshotBudgetLease() noexcept = default;
  bool valid() const noexcept { return receipt_ != nullptr; }
  std::uint64_t chargedBytes() const noexcept {
    return receipt_ ? receipt_->chargedBytes : 0u;
  }
  std::uint64_t resourceBytes() const noexcept {
    return receipt_ ? receipt_->resourceBytes : 0u;
  }
  RenderTapeSnapshotBudgetKind kind() const noexcept {
    return receipt_ ? receipt_->kind : RenderTapeSnapshotBudgetKind::Retained;
  }
  // A receipt may accompany moves/shares of ONE allocation, not a second
  // equal-sized copy. This pointer is process-local, never dereferenced or
  // serialized. Adoption verifies a PE-side binding rather than making one.
  bool bindRetainedPayload(const void* data, std::uint64_t bytes) const noexcept {
    if (!receipt_ || !data || bytes == 0u ||
        receipt_->kind != RenderTapeSnapshotBudgetKind::Retained ||
        receipt_->resourceBytes != bytes || receipt_->chargedBytes != bytes)
      return false;
    std::lock_guard lock(receipt_->state->mutex);
    if (receipt_->payloadBound) return receipt_->boundPayload == data;
    receipt_->boundPayload = data;
    receipt_->payloadBound = true;
    return true;
  }
  bool boundPayloadMatches(const void* data, std::uint64_t bytes) const noexcept {
    if (!receipt_ || !data || bytes == 0u ||
        receipt_->kind != RenderTapeSnapshotBudgetKind::Retained ||
        receipt_->resourceBytes != bytes || receipt_->chargedBytes != bytes)
      return false;
    std::lock_guard lock(receipt_->state->mutex);
    return receipt_->payloadBound && receipt_->boundPayload == data;
  }
  void reset() noexcept { receipt_.reset(); }
private:
  friend class RenderTapeSnapshotBudget;
  explicit RenderTapeSnapshotBudgetLease(
      std::shared_ptr<const snapshot_budget_detail::Receipt> receipt) noexcept
      : receipt_(std::move(receipt)) {}
  std::shared_ptr<const snapshot_budget_detail::Receipt> receipt_{};
};

struct RenderTapeSnapshotBudgetReservation {
  RenderTapeSnapshotBudgetStatus status =
      RenderTapeSnapshotBudgetStatus::NotInitialized;
  RenderTapeSnapshotBudgetLease lease{};

  explicit operator bool() const noexcept {
    return status == RenderTapeSnapshotBudgetStatus::Complete && lease.valid();
  }
};

// This facade is cold and never crosses the PE/unix ABI. Receipts own their
// shared state, so final release remains safe after the facade is destroyed.
// There is deliberately no reset/reconfigure operation, even with no leases.
class RenderTapeSnapshotBudget {
public:
  RenderTapeSnapshotBudget() noexcept = default;
  RenderTapeSnapshotBudget(const RenderTapeSnapshotBudget&) = delete;
  RenderTapeSnapshotBudget& operator=(const RenderTapeSnapshotBudget&) = delete;

  RenderTapeSnapshotBudgetStatus initialize(
      std::uint64_t normalizedMaxBlobBytes) noexcept {
    return initialize(renderTapeDeriveSnapshotBudgetLimits(normalizedMaxBlobBytes));
  }
  RenderTapeSnapshotBudgetStatus initialize(
      RenderTapeSnapshotBudgetLimits limits) noexcept {
    try {
      std::lock_guard lock(mutex_);
      if (state_) return RenderTapeSnapshotBudgetStatus::AlreadyInitialized;
      if (!renderTapeSnapshotBudgetLimitsValid(limits))
        return RenderTapeSnapshotBudgetStatus::InvalidLimits;
      state_ = std::make_shared<snapshot_budget_detail::SharedState>(limits);
      return RenderTapeSnapshotBudgetStatus::Complete;
    } catch (const std::bad_alloc&) {
      return RenderTapeSnapshotBudgetStatus::AllocationFailed;
    } catch (...) {
      return RenderTapeSnapshotBudgetStatus::InternalFailure;
    }
  }
  bool initialized() const noexcept { return sharedState() != nullptr; }
  RenderTapeSnapshotBudgetLimits limits() const noexcept {
    const auto state = sharedState();
    if (!state) return {};
    std::lock_guard lock(state->mutex);
    return state->value.limits;
  }
  RenderTapeSnapshotBudgetGauges gauges() const noexcept {
    const auto state = sharedState();
    if (!state) return {};
    std::lock_guard lock(state->mutex);
    return state->value.gauges;
  }
  RenderTapeSnapshotBudgetReservation reserveRetained(
      std::uint64_t bytes) noexcept {
    return reserve(RenderTapeSnapshotBudgetKind::Retained, bytes, bytes);
  }
  RenderTapeSnapshotBudgetReservation reserveTransient(
      std::uint64_t resourceBytes, std::uint64_t stagingBytes) noexcept {
    return reserve(RenderTapeSnapshotBudgetKind::Transient,
                   resourceBytes, stagingBytes);
  }
private:
  std::shared_ptr<snapshot_budget_detail::SharedState> sharedState() const {
    std::lock_guard lock(mutex_);
    return state_;
  }
  RenderTapeSnapshotBudgetReservation reserve(
      RenderTapeSnapshotBudgetKind kind, std::uint64_t resourceBytes,
      std::uint64_t chargedBytes) noexcept {
    try {
      const auto state = sharedState();
      if (!state) return {RenderTapeSnapshotBudgetStatus::NotInitialized, {}};
      std::lock_guard lock(state->mutex);
      auto next = state->value;
      const auto status = renderTapeSnapshotBudgetAdmit(
          next, kind, resourceBytes, chargedBytes);
      if (status != RenderTapeSnapshotBudgetStatus::Complete)
        return {status, {}};
      // All fallible receipt metadata allocation precedes the charge. An
      // allocation failure leaves current AND peak exactly as they were.
      auto receipt = std::make_shared<snapshot_budget_detail::Receipt>(
          state, kind, resourceBytes, chargedBytes);
      state->value = next;
      receipt->active = true;
      return {RenderTapeSnapshotBudgetStatus::Complete,
              RenderTapeSnapshotBudgetLease(std::move(receipt))};
    } catch (const std::bad_alloc&) {
      return {RenderTapeSnapshotBudgetStatus::AllocationFailed, {}};
    } catch (...) {
      return {RenderTapeSnapshotBudgetStatus::InternalFailure, {}};
    }
  }
  mutable std::mutex mutex_{};
  std::shared_ptr<snapshot_budget_detail::SharedState> state_{};
};

} // namespace dxmt9::d3d9
