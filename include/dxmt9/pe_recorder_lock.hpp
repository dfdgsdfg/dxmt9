#pragma once

#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>

#include "dxmt9/thread_ownership.hpp"

namespace dxmt9::d3d9::pe {

enum class RecorderAccessLane : std::uint8_t {
  Denied,
  Owner,
  Locked,
};

struct RecorderAccessFacts {
  bool lockRequired = false;
  bool lockHeld = false;
  bool ownerThread = false;
};

// One production predicate admits both legal PE lanes.  The ordinary device
// lane is producer-owned and does not touch the mutex; D3DCREATE_MULTITHREADED
// (or the explicit force-lock rollback lane) is admitted only by an actually
// held RecorderLockGuard.  A policy bool by itself is not a lock witness.
constexpr RecorderAccessLane planRecorderAccess(
    RecorderAccessFacts facts) noexcept {
  if (facts.lockRequired) {
    return facts.lockHeld ? RecorderAccessLane::Locked
                          : RecorderAccessLane::Denied;
  }
  return facts.ownerThread ? RecorderAccessLane::Owner
                           : RecorderAccessLane::Denied;
}

class RecorderLockCapability;

// A call-scope borrow is deliberately neither copyable nor movable.  It may
// be visited only through a nothrow callback and rechecks the recorder epoch
// before exposing the source for that callback.  SparseStatePlan embeds these
// borrows, so the plan cannot be captured by value into deferred replay.
template <typename T>
class RecorderBorrow final {
 public:
  RecorderBorrow() noexcept = default;
  RecorderBorrow(const RecorderBorrow&) = delete;
  RecorderBorrow& operator=(const RecorderBorrow&) = delete;
  RecorderBorrow(RecorderBorrow&&) = delete;
  RecorderBorrow& operator=(RecorderBorrow&&) = delete;

  bool valid() const noexcept {
    return value_ != nullptr && epoch_ != nullptr && *epoch_ == issuedEpoch_ &&
           lane_ != RecorderAccessLane::Denied;
  }

  template <typename Fn>
    requires std::is_nothrow_invocable_v<Fn&&, T&>
  bool with(Fn&& fn) const noexcept {
    if (!valid())
      return false;
    std::forward<Fn>(fn)(*value_);
    return true;
  }

  void invalidate() noexcept {
    value_ = nullptr;
    epoch_ = nullptr;
    issuedEpoch_ = 0u;
    lane_ = RecorderAccessLane::Denied;
  }

 private:
  void bind(const RecorderLockCapability& capability, T& value) noexcept;

  T* value_ = nullptr;
  const std::uint64_t* epoch_ = nullptr;
  std::uint64_t issuedEpoch_ = 0u;
  RecorderAccessLane lane_ = RecorderAccessLane::Denied;

  friend class RecorderLockCapability;
};

// Mutation/settlement authority paired with RecorderBorrow.  Epoch ownership
// remains in the recorder; Reset/poison advances it and every outstanding
// capability then fails closed without retaining another owner.
class RecorderLockCapability final {
 public:
  RecorderLockCapability(const RecorderLockCapability&) = delete;
  RecorderLockCapability& operator=(const RecorderLockCapability&) = delete;
  RecorderLockCapability(RecorderLockCapability&&) = delete;
  RecorderLockCapability& operator=(RecorderLockCapability&&) = delete;

  bool valid() const noexcept {
    return epoch_ != nullptr && *epoch_ == issuedEpoch_ &&
           lane_ != RecorderAccessLane::Denied;
  }

  RecorderAccessLane lane() const noexcept {
    return valid() ? lane_ : RecorderAccessLane::Denied;
  }

  std::uint64_t epoch() const noexcept {
    return valid() ? issuedEpoch_ : 0u;
  }

  template <typename T>
  bool bind(RecorderBorrow<T>& borrow, T& value) const noexcept {
    if (!valid()) {
      borrow.invalidate();
      return false;
    }
    borrow.bind(*this, value);
    return true;
  }

 private:
  RecorderLockCapability(RecorderAccessLane lane,
                         const std::uint64_t& epoch) noexcept
      : epoch_(&epoch), issuedEpoch_(epoch), lane_(lane) {}

  const std::uint64_t* epoch_ = nullptr;
  std::uint64_t issuedEpoch_ = 0u;
  RecorderAccessLane lane_ = RecorderAccessLane::Denied;

  template <typename T>
  friend class RecorderBorrow;
  friend class RecorderLockGuard;
};

template <typename T>
void RecorderBorrow<T>::bind(const RecorderLockCapability& capability,
                             T& value) noexcept {
  value_ = &value;
  epoch_ = capability.epoch_;
  issuedEpoch_ = capability.issuedEpoch_;
  lane_ = capability.lane_;
}

// The PE recorder is single-producer by default.  This guard is the one
// production ownership witness used by both the append envelope and the
// cold state-block/tape intervals.  Keeping the branch in this small type is
// important: the unlocked path must not construct a lock operation or touch
// an atomic merely because a setter was entered.
class RecorderLockGuard final {
 public:
  RecorderLockGuard(std::recursive_mutex& mutex, bool required) noexcept
      : mutex_(mutex), required_(required) {
    if (required_)
      mutex_.lock();
  }

  ~RecorderLockGuard() {
    if (required_)
      mutex_.unlock();
  }

  RecorderLockGuard(const RecorderLockGuard&) = delete;
  RecorderLockGuard& operator=(const RecorderLockGuard&) = delete;

  bool ownsLock() const noexcept { return required_; }

  RecorderLockCapability capability(
      const dxmt9::core::ThreadOwnershipToken& owner,
      const std::uint64_t& epoch) const noexcept {
    return RecorderLockCapability(
        planRecorderAccess({
            .lockRequired = required_,
            .lockHeld = ownsLock(),
            .ownerThread = owner.ownedByCurrentThread(),
        }),
        epoch);
  }

 private:
  std::recursive_mutex& mutex_;
  bool required_;
};

constexpr bool recorderLockRequired(std::uint32_t behaviorFlags,
                                    bool forceLock) noexcept {
  constexpr std::uint32_t kD3DCreateMultithreaded = 0x00000004u;
  return (behaviorFlags & kD3DCreateMultithreaded) != 0u || forceLock;
}

}  // namespace dxmt9::d3d9::pe
