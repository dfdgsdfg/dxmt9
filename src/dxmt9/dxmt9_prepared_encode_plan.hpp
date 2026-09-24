#pragma once

#include "dxmt9_parallel_render_pass.hpp"
#include <memory>
#include <new>
#include <utility>

namespace dxmt9::encoders {

// Value foundation: publication shadow observes this; no runtime async owner yet.
struct PreparedSourceKey {
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t sequence = 0;
  std::uint32_t firstCommand = 0, commandCount = 0;
  constexpr bool valid() const noexcept {
    return source.valid() && sequence && commandCount &&
        firstCommand <= UINT32_MAX - commandCount;
  }
  friend constexpr bool operator==(const PreparedSourceKey&,
                                   const PreparedSourceKey&) = default;
};

enum class PreparedAccessResolution : std::uint8_t {
  CanonicalStable, RequiresEncodeResolution,
};
enum class PreparedPlanFailure : std::uint8_t {
  None, InvalidSource, Allocation, Capacity, CommandOrder, InvalidAccess,
  UnsupportedCommand, Incomplete, Consumed,
};
inline constexpr std::uint32_t kPreparedIdentityCapacity = 512;
inline constexpr std::uint32_t kPreparedLookupCapacity = 1024;
static_assert(kPreparedLookupCapacity == 2 * kPreparedIdentityCapacity);
static_assert((kPreparedLookupCapacity & (kPreparedLookupCapacity - 1)) == 0);
static_assert(kPreparedIdentityCapacity < UINT16_MAX);
// Full-width mixing; bucket equality is never identity equality.
constexpr std::uint32_t preparedIdentityBucket(std::uint64_t key) noexcept {
  key ^= key >> 30;
  key *= UINT64_C(0xbf58476d1ce4e5b9);
  key ^= key >> 27;
  key *= UINT64_C(0x94d049bb133111eb);
  key ^= key >> 31;
  return static_cast<std::uint32_t>(key) & (kPreparedLookupCapacity - 1);
}
struct PreparedPlanLimits {
  std::uint32_t identities = kPreparedIdentityCapacity, commands = 1024, accesses = 8192, ranges = 1024;
  constexpr bool valid() const noexcept {
    return identities <= kPreparedIdentityCapacity && commands <= 1024 && accesses <= 8192 && ranges <= 1024;
  }
};
struct PreparedCommandAccess {
  core::MetalCommandKind kind{};
  std::uint32_t readOffset = 0, readCount = 0, writeOffset = 0, writeCount = 0;
  PreparedAccessResolution resolution = PreparedAccessResolution::RequiresEncodeResolution;
};
// Maximal same-attachment DrawRun ranges, or one coordinator command. These
// are candidates only, not child execution or pass-action authority.
struct PreparedCommandRange {
  std::uint32_t first = 0, count = 0;
  ParallelPassCommandRole role = ParallelPassCommandRole::Unsupported;
  core::RenderAttachmentKey attachments{};
};
struct PreparedEncodePlanStorage {
  PreparedSourceKey key{};
  std::array<std::uint64_t, kPreparedIdentityCapacity> identities{};
  std::array<std::uint16_t, 8192> accesses{};
  std::array<PreparedCommandAccess, 1024> commands{};
  std::array<PreparedCommandRange, 1024> ranges{};
  std::uint32_t identityCount = 0, accessCount = 0, commandCount = 0, rangeCount = 0;
  bool needsEncodeResolution = false;
};
static_assert(std::is_trivially_copyable_v<PreparedEncodePlanStorage>);
static_assert(std::is_standard_layout_v<PreparedEncodePlanStorage>);

// Structural closure only: not an alias-stability or parallel-admission proof.
inline bool validatePreparedEncodePlan(const PreparedEncodePlanStorage& s,
                                      PreparedSourceKey expected) noexcept {
  if (!expected.valid() || s.key != expected || s.commandCount != expected.commandCount ||
      s.commandCount > s.commands.size() || s.identityCount > s.identities.size() ||
      s.accessCount > s.accesses.size() || !s.rangeCount || s.rangeCount > s.ranges.size())
    return false;
  for (std::uint32_t i = 0; i < s.identityCount; ++i) {
    if (!s.identities[i]) return false;
    for (std::uint32_t j = 0; j < i; ++j)
      if (s.identities[i] == s.identities[j]) return false;
  }
  std::array<bool, kPreparedIdentityCapacity> used{};
  std::uint32_t cursor = 0;
  bool unresolved = false;
  for (std::uint32_t i = 0; i < s.commandCount; ++i) {
    const auto& row = s.commands[i];
    if (classifyParallelPassCommandRole(row.kind) == ParallelPassCommandRole::Unsupported ||
        row.readCount > core::kSourceEntryResourceCapacity ||
        row.writeCount > core::kSourceEntryResourceCapacity ||
        row.readOffset != cursor || row.readCount > s.accessCount - cursor)
      return false;
    cursor += row.readCount;
    if (row.writeOffset != cursor || row.writeCount > s.accessCount - cursor) return false;
    cursor += row.writeCount;
    if (row.resolution == PreparedAccessResolution::RequiresEncodeResolution) {
      unresolved = true;
      if (row.readCount || row.writeCount) return false;
    } else if (row.resolution != PreparedAccessResolution::CanonicalStable) return false;
    for (const auto range : {std::pair{row.readOffset, row.readCount},
                             std::pair{row.writeOffset, row.writeCount}}) {
      for (std::uint32_t at = range.first; at < range.first + range.second; ++at) {
        if (s.accesses[at] >= s.identityCount) return false;
        used[s.accesses[at]] = true;
        for (std::uint32_t prior = range.first; prior < at; ++prior)
          if (s.accesses[prior] == s.accesses[at]) return false;
      }
    }
  }
  if (cursor != s.accessCount || unresolved != s.needsEncodeResolution) return false;
  for (std::uint32_t i = 0; i < s.identityCount; ++i) if (!used[i]) return false;
  cursor = 0;
  for (std::uint32_t i = 0; i < s.rangeCount; ++i) {
    const auto& range = s.ranges[i];
    if (range.first != cursor || !range.count || range.count > s.commandCount - cursor ||
        (range.role != ParallelPassCommandRole::Draw && range.count != 1)) return false;
    for (std::uint32_t j = cursor; j < cursor + range.count; ++j)
      if (classifyParallelPassCommandRole(s.commands[j].kind) != range.role) return false;
    cursor += range.count;
  }
  return cursor == s.commandCount;
}

class PreparedEncodePlanBuilder;
class PreparedEncodePlanWorkspace;
class PreparedEncodePlan {
 public:
  PreparedEncodePlan() = default;
  PreparedEncodePlan(PreparedEncodePlan&&) noexcept = default;
  PreparedEncodePlan& operator=(PreparedEncodePlan&&) noexcept = default;
  PreparedEncodePlan(const PreparedEncodePlan&) = delete;
  PreparedEncodePlan& operator=(const PreparedEncodePlan&) = delete;
  // A caller must hold this owner for the whole synchronous read. Persisted
  // metadata contains only indices; this view is not an async borrow token.
  const PreparedEncodePlanStorage* view(const PreparedSourceKey& key) const & noexcept {
    return storage_ && storage_->key == key ? storage_.get() : nullptr;
  }
  const PreparedEncodePlanStorage* view(const PreparedSourceKey&) const && = delete;
  explicit operator bool() const noexcept { return storage_ != nullptr; }
 private:
  friend class PreparedEncodePlanBuilder;
  friend class PreparedEncodePlanWorkspace;
  std::unique_ptr<PreparedEncodePlanStorage> storage_{};
};

class PreparedEncodePlanBuilder {
 public:
  // One owner allocation, never one allocation per command. The injected
  // allocator is used only at construction, not retained by the builder.
  using Allocator = std::unique_ptr<PreparedEncodePlanStorage> (*)() noexcept;
  static std::unique_ptr<PreparedEncodePlanStorage> allocate() noexcept {
    return std::unique_ptr<PreparedEncodePlanStorage>(
        new (std::nothrow) PreparedEncodePlanStorage{});
  }
  explicit PreparedEncodePlanBuilder(PreparedSourceKey key,
      PreparedPlanLimits limits = {}, Allocator allocator = allocate) noexcept
      : limits_(limits) {
    if (!key.valid()) { fail(PreparedPlanFailure::InvalidSource); return; }
    if (!limits.valid() || key.commandCount > limits.commands) {
      fail(PreparedPlanFailure::Capacity); return;
    }
    storage_ = allocator ? allocator() : nullptr;
    if (!storage_) { fail(PreparedPlanFailure::Allocation); return; }
    storage_->key = key;
  }
  PreparedEncodePlanBuilder(const PreparedEncodePlanBuilder&) = delete;
  PreparedEncodePlanBuilder& operator=(const PreparedEncodePlanBuilder&) = delete;
  PreparedPlanFailure failure() const noexcept { return failure_; }

  bool append(std::uint32_t commandIndex, core::MetalCommandKind kind,
      const core::ExactResourceSet& reads, const core::ExactResourceSet& writes,
      core::RenderAttachmentKey attachments,
      PreparedAccessResolution resolution) noexcept {
    if (!storage_ || failure_ != PreparedPlanFailure::None) return false;
    auto& s = *storage_;
    if (s.commandCount == s.key.commandCount ||
        commandIndex != s.key.firstCommand + s.commandCount)
      return fail(PreparedPlanFailure::CommandOrder);
    const auto role = classifyParallelPassCommandRole(kind);
    if (role == ParallelPassCommandRole::Unsupported)
      return fail(PreparedPlanFailure::UnsupportedCommand);
    if (resolution != PreparedAccessResolution::CanonicalStable &&
        resolution != PreparedAccessResolution::RequiresEncodeResolution)
      return fail(PreparedPlanFailure::InvalidAccess);
    const bool canonical = resolution == PreparedAccessResolution::CanonicalStable;
    if (canonical && (!parallelPassExactResourceSetValid(reads) ||
                      !parallelPassExactResourceSetValid(writes)))
      return fail(PreparedPlanFailure::InvalidAccess);
    const bool extend = s.rangeCount && role == ParallelPassCommandRole::Draw &&
        s.ranges[s.rangeCount - 1].role == role &&
        s.ranges[s.rangeCount - 1].attachments == attachments;
    if (!extend && s.rangeCount == limits_.ranges)
      return fail(PreparedPlanFailure::Capacity);
    PreparedCommandAccess command{.kind = kind, .resolution = resolution};
    command.readOffset = s.accessCount;
    if (canonical) {
      if (!appendAccesses(reads)) return false;
      command.readCount = reads.count;
    }
    command.writeOffset = s.accessCount;
    if (canonical) {
      if (!appendAccesses(writes)) return false;
      command.writeCount = writes.count;
    }
    // An unresolved row keeps its exact source command position. No guessed
    // canonical identity is emitted; Encode must re-resolve or select serial.
    s.needsEncodeResolution |= !canonical;
    if (extend) ++s.ranges[s.rangeCount - 1].count;
    else s.ranges[s.rangeCount++] = {s.commandCount, 1, role, attachments};
    s.commands[s.commandCount++] = command;
    return true;
  }

  PreparedEncodePlan seal() noexcept {
    PreparedEncodePlan result;
    if (!storage_ || failure_ != PreparedPlanFailure::None) return result;
    if (storage_->commandCount != storage_->key.commandCount) {
      fail(PreparedPlanFailure::Incomplete);
      return result;
    }
    if (!validatePreparedEncodePlan(*storage_, storage_->key)) {
      fail(PreparedPlanFailure::InvalidAccess);
      return result;
    }
    result.storage_ = std::move(storage_);
    failure_ = PreparedPlanFailure::Consumed;
    return result;
  }
 private:
  friend class PreparedEncodePlanWorkspace;
  PreparedEncodePlanBuilder(PreparedSourceKey key, PreparedPlanLimits limits,
      std::unique_ptr<PreparedEncodePlanStorage> storage, Allocator allocator) noexcept
      : storage_(std::move(storage)), limits_(limits), recyclable_(true) {
    if (!key.valid()) { fail(PreparedPlanFailure::InvalidSource); return; }
    if (!limits.valid() || key.commandCount > limits.commands) {
      fail(PreparedPlanFailure::Capacity); return;
    }
    if (!storage_) storage_ = allocator ? allocator() : nullptr;
    if (!storage_) { fail(PreparedPlanFailure::Allocation); return; }
    // Old array tails are inaccessible: append overwrites each counted row,
    // and every reader is bounded by these freshly reset cursors.
    storage_->key = key;
    storage_->identityCount = storage_->accessCount = 0;
    storage_->commandCount = storage_->rangeCount = 0;
    storage_->needsEncodeResolution = false;
  }
  bool fail(PreparedPlanFailure reason) noexcept {
    if (failure_ == PreparedPlanFailure::None) failure_ = reason;
    // A recyclable failed prefix stays private and cannot append or seal.
    if (!recyclable_) storage_.reset();
    return false;
  }
  bool appendAccesses(const core::ExactResourceSet& set) noexcept {
    for (std::uint32_t i = 0; i < set.count; ++i) {
      auto& s = *storage_;
      if (s.accessCount == limits_.accesses) return fail(PreparedPlanFailure::Capacity);
      auto bucket = preparedIdentityBucket(set.handles[i]);
      bool found = false;
      for (std::uint32_t probe = 0; probe < kPreparedLookupCapacity; ++probe) {
        auto& entry = lookup_[bucket];
        if (!entry) {
          if (s.identityCount == limits_.identities) return fail(PreparedPlanFailure::Capacity);
          const auto index = s.identityCount++;
          s.identities[index] = set.handles[i];
          entry = static_cast<std::uint16_t>(index + 1);
          s.accesses[s.accessCount++] = static_cast<std::uint16_t>(index);
          found = true;
          break;
        }
        const auto index = static_cast<std::uint32_t>(entry - 1);
        if (s.identities[index] == set.handles[i]) {
          s.accesses[s.accessCount++] = static_cast<std::uint16_t>(index);
          found = true;
          break;
        }
        bucket = (bucket + 1) & (kPreparedLookupCapacity - 1);
      }
      if (!found) return fail(PreparedPlanFailure::Capacity);
    }
    return true;
  }
  std::unique_ptr<PreparedEncodePlanStorage> storage_{};
  // Reset with each builder, including workspace acquire. Never sealed or
  // shared with Encode; values reference this builder's dense dictionary only.
  std::array<std::uint16_t, kPreparedLookupCapacity> lookup_{};
  PreparedPlanLimits limits_{};
  PreparedPlanFailure failure_ = PreparedPlanFailure::None;
  bool recyclable_ = false;
};

// Synchronous diagnostic scratch, not a cache of source semantics. Exclusive
// unique ownership moves out at acquire and back only after all views expire.
// No builder/plan holds a back-pointer: it may safely outlive the workspace.
class PreparedEncodePlanWorkspace {
 public:
  bool hasStorage() const noexcept { return storage_ != nullptr; }
  PreparedEncodePlanBuilder acquire(PreparedSourceKey key,
      PreparedPlanLimits limits = {},
      PreparedEncodePlanBuilder::Allocator allocator = PreparedEncodePlanBuilder::allocate) noexcept {
    return PreparedEncodePlanBuilder(key, limits, std::move(storage_), allocator);
  }
  void recycle(PreparedEncodePlan&& plan) noexcept {
    if (plan.storage_) storage_ = std::move(plan.storage_);
  }
  void recycle(PreparedEncodePlanBuilder&& builder) noexcept {
    if (builder.storage_) storage_ = std::move(builder.storage_);
    builder.failure_ = PreparedPlanFailure::Consumed;
  }
 private:
  std::unique_ptr<PreparedEncodePlanStorage> storage_{};
};

// Pure publication/lifetime reducer. The future queue adapter must apply its
// accepted state under the queue's publication lock/release-acquire contract.
// This value model alone neither publishes a live Slot nor owns GPU resources.
enum class PreparedPublicationPhase : std::uint8_t {
  Preparing, Published, Encoding, Joined, Submitted, Completed, Retired, Aborted,
};
enum class PreparedPublicationAction : std::uint8_t {
  SlotReady, PlanReady, PlanFailed, Publish, Abort, SelectSerial, BeginEncode,
  Child0Done, Child1Done, Join, EncodeFailed, ReleasePlan, Submit, Complete,
  RetireSource, Count,
};
struct PreparedPublicationState {
  PreparedSourceKey key{};
  PreparedPublicationPhase phase = PreparedPublicationPhase::Preparing;
  bool slotReady = false, planReady = false, planFailed = false;
  bool prepared = false, planReleased = false, encodeFailed = false;
  std::uint8_t childrenDone = 0;
  friend constexpr bool operator==(const PreparedPublicationState&,
                                   const PreparedPublicationState&) = default;
};
struct PreparedPublicationStep {
  PreparedPublicationState state{};
  bool accepted = false;
};
constexpr PreparedPublicationStep reducePreparedPublication(
    PreparedPublicationState input, PreparedPublicationAction action,
    PreparedSourceKey witness) noexcept {
  using P = PreparedPublicationPhase;
  using A = PreparedPublicationAction;
  PreparedPublicationStep result{input, false};
  if (!input.key.valid() || witness != input.key) return result;
  auto& s = result.state;
  const bool preparing = s.phase == P::Preparing;
  switch (action) {
  case A::SlotReady:
    if (!preparing || s.slotReady) return result;
    s.slotReady = true; break;
  case A::PlanReady:
    if (!preparing || s.planReady || s.planFailed) return result;
    s.planReady = true; break;
  case A::PlanFailed:
    if (!preparing || s.planFailed) return result;
    s.planReady = false; s.planFailed = true; break;
  case A::Publish:
    if (!preparing || !s.slotReady || (!s.planReady && !s.planFailed)) return result;
    s.prepared = s.planReady;
    s.planReleased = !s.prepared;
    s.phase = P::Published; break;
  case A::Abort:
    if (!preparing) return result;
    s.phase = P::Aborted; s.planReleased = true; break;
  case A::SelectSerial:
    if (s.phase != P::Published || !s.prepared) return result;
    s.prepared = false; s.planReleased = true; break;
  case A::BeginEncode:
    if (s.phase != P::Published) return result;
    s.phase = P::Encoding; break;
  case A::Child0Done:
  case A::Child1Done: {
    const auto bit = action == A::Child0Done ? 1u : 2u;
    if (s.phase != P::Encoding || (!s.prepared && bit == 2u) ||
        (s.childrenDone & bit)) return result;
    s.childrenDone |= bit; break;
  }
  case A::Join:
    if (s.phase != P::Encoding || s.childrenDone != (s.prepared ? 3u : 1u)) return result;
    s.phase = P::Joined; break;
  case A::EncodeFailed:
    if (s.phase != P::Encoding || s.encodeFailed) return result;
    s.encodeFailed = true; break;
  case A::ReleasePlan:
    if (s.planReleased || (s.phase != P::Joined && s.phase != P::Submitted &&
                          s.phase != P::Completed)) return result;
    s.planReleased = true; break;
  case A::Submit:
    if (s.phase != P::Joined) return result;
    s.phase = P::Submitted; break;
  case A::Complete:
    if (s.phase != P::Submitted) return result;
    s.phase = P::Completed; break;
  case A::RetireSource:
    if (s.phase != P::Completed || !s.planReleased) return result;
    s.phase = P::Retired; break;
  default: return result;
  }
  result.accepted = true;
  return result;
}

} // namespace dxmt9::encoders
