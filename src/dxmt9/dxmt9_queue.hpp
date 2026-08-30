#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9_capture.hpp"
#include "dxmt9_cpu_ready_tape.hpp"
#include "dxmt9_post_encode_retirement.hpp"
#include "dxmt9_pipeline_lifecycle.hpp"
#include "render/encode_scheduling_progress.hpp"
#include "../winemetal/Metal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <source_location>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include <deque>
#include <condition_variable>
#include <mutex>

namespace dxmt9::core::metalhud {
class SubmissionDiagnosticsController;
}

namespace dxmt9 {
class CommandQueue;
class SchedulingProgressWatchdog;
namespace render {
class FrameGraphBackend;
}
}

namespace dxmt9::core::metalqueue {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct CpuReadySupplyObservationToken {
  u64 value = 0;

  constexpr bool valid() const noexcept { return value != 0; }
  friend constexpr bool operator==(
      CpuReadySupplyObservationToken,
      CpuReadySupplyObservationToken) noexcept = default;
};

constexpr u64 committedSequenceWaitTarget(u64 requestedSeqId,
                                          u64 lastCommittedSeqId) noexcept {
  return requestedSeqId <= lastCommittedSeqId ? requestedSeqId
                                               : lastCommittedSeqId;
}

enum class RenderEncoderGpuPassType : u32 {
  Unknown = 0,
  Draw,
  Clear,
  SurfaceCopy,
  StretchRect,
  ColorFill,
  DepthResolve,
  Present,
};

struct CommandBufferDiagnostics {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  bool hasStretchRect = false;
  u32 frame = 0;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

enum class QueueSlotState {
  Free = 0,
  Writing = 1,
  Pending = 2,
  Encoding = 3,
  Retiring = 4,
  GPU = 5,
};

struct ActiveSlotInfo {
  size_t index = 0;
  QueueSlotState state = QueueSlotState::Free;
  u64 seqId = 0;
  size_t commandCount = 0;
};

struct QueueTraceSnapshot {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct ChunkSummaryInput {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  bool hasStretchRect = false;
  u32 frame = 0;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

enum class ChunkObservationKind {
  Draw,
  Blit,
  StretchRect,
  Present,
};

enum class ReplayCategory : std::uint8_t {
  Draw,
  Copy,
  Present,
};

// Effective encode replay observation. All fields are values except the
// resourceHandles span, which is valid only for the synchronous sink call.
// A sink must not retain the record or its span. Source identity carries both
// the source-slot and storage generations, making recycled queue storage
// distinguishable from the represented source that occupied it.
struct ReplayObservation {
  CpuReadyTape::SourceRef source{};
  u64 seqId = 0;
  u32 commandOrdinal = 0;
  MetalCommandKind commandKind = MetalCommandKind::DrawRun;
  ReplayCategory category = ReplayCategory::Draw;
  bool barrier = false;
  bool readback = false;
  std::span<const ChunkHandleEntry> resourceHandles{};
};

using ReplayObserverFn = void (*)(void* context,
                                  const ReplayObservation& observation) noexcept;

struct ReplayObserverSink {
  void* context = nullptr;
  ReplayObserverFn fn = nullptr;

  explicit operator bool() const noexcept { return fn != nullptr; }
};

struct ChunkObservation {
  ChunkObservationKind kind = ChunkObservationKind::Draw;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

struct QueueTraceState {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct QueueLifecycleContext {
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
};

struct QueueControllerState {
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  std::span<const ChunkSlotControl> slots;
};

enum class QueueLifecycleEvent {
  PresentEnqueue,
  WriterWaitBegin,
  WriterWaitEnd,
  WriterAcquire,
  CommitEmpty,
  CommitWaitBegin,
  CommitWaitEnd,
  CommitPublish,
  EncodeDequeue,
  EncodeCommit,
  GpuComplete,
  FinishInline,
  FinishDequeue,
  ReclaimFree,
  WaitSeqBegin,
  WaitSeqEnd,
};

struct QueueTransitionRecord {
  QueueControllerState before{};
  QueueControllerState after{};
  std::optional<size_t> slotIndex;
  u64 eventSeqId = 0;
  size_t inflightLimit = 0;
  const SwapDesc* present = nullptr;
  Handle sourceHandle{};
  // Scalar slot witnesses keep diagnostic classification truthful even though
  // QueueControllerState intentionally borrows the live slot array.
  std::optional<ChunkSlot::State> beforeSlotState{};
  std::optional<ChunkSlot::State> afterSlotState{};
  std::optional<QueueLifecycleEvent> forcedEvent{};
  std::size_t beforeCommandCount = 0;
  std::size_t afterCommandCount = 0;
  CpuReadySourceId beforeSourceId{};
  CpuReadyStorageRef beforeStorage{};
  CpuReadySourceId afterSourceId{};
  CpuReadyStorageRef afterStorage{};
  // Reclaiming deliberately rejects the generic tape metadata/payload view.
  // Preserve the pre-transition witness so owner reporting remains possible
  // after an inline completion has detached the source from its slot.
  std::optional<CpuReadySourceMetadata> beforeMetadata{};
  std::optional<CpuReadyTape::PayloadKind> beforePayloadKind{};
  std::optional<bool> beforePresentOnly{};
};

struct NoEnqueueCommitChunkRecordShape {
  u64 recordCount = 0;
  u64 drawRecords = 0;
  u64 constRecords = 0;
  u64 applyStateRecords = 0;
  u64 clearRecords = 0;
  u64 presentRecords = 0;
  u64 surfaceRecords = 0;
  u64 queryRecords = 0;
  u64 otherRecords = 0;
};

struct NoEnqueueFirstPublishSlotShape {
  u64 commandCount = 0;
  u64 drawRunCommands = 0;
  u64 drawItems = 0;
  u64 nonDrawCommands = 0;
  u64 payloadBytes = 0;
  u64 presentCommands = 0;
  u64 prePresentCommands = 0;
  u64 prePresentDrawRunCommands = 0;
  u64 prePresentDrawItems = 0;
  u64 prePresentNonDrawCommands = 0;
  u64 prePresentPayloadBytes = 0;
  u64 postPresentCommands = 0;
  u64 presentTailSlots = 0;
  u64 presentNonTailSlots = 0;
};

NoEnqueueFirstPublishSlotShape summarizeNoEnqueueFirstPublishSlotShape(
    const ChunkSlot& slot) noexcept;

struct QueueCompletionSource {
  CpuReadyTape::SourceRef source{};
  // Valid only after the queue has synchronously encoded and physically
  // retired this source. Exactly one of source/receipt is authoritative.
  PostEncodeCompletionReceipt receipt{};
  // Temporary control index is retained for diagnostics only. Completion and
  // reclaim identity is source + seqId and never resolves this live shell.
  size_t slotIndex = 0;
  u64 seqId = 0;
  bool hasPresent = false;
  size_t commandBegin = 0;
  size_t commandCount = 0;

  bool locatorBacked() const noexcept {
    return source.valid() && !receipt.valid();
  }
  bool receiptBacked() const noexcept {
    return !source.valid() && receipt.valid() && receipt.seqId == seqId;
  }
  bool completionIdentityValid() const noexcept {
    return seqId != 0u && (locatorBacked() || receiptBacked());
  }
};

// Fixed-capacity value ledger for completed SegmentSerial event tails. The
// consumer must drain this ledger; an unconsumed producer cannot grow it
// without bound and fails closed on overflow. Tail seqIds are monotonic so a
// wrapped/ABA publication cannot masquerade as a newer event settlement.
struct ArenaGroupSettlementLedger {
  static constexpr std::size_t kCapacity = 128u;
  std::array<CpuReadyTape::ArenaGroupSettlement, kCapacity> entries{};
  std::size_t head = 0;
  std::size_t count = 0;
  std::uint64_t lastAppendedTailSeqId = 0;
  std::uint64_t lastConsumedTailSeqId = 0;

  bool append(CpuReadyTape::ArenaGroupSettlement settlement) noexcept {
    if (!settlement.valid() || count == kCapacity ||
        settlement.tailSeqId <= lastAppendedTailSeqId) {
      return false;
    }
    entries[(head + count) % kCapacity] = settlement;
    ++count;
    lastAppendedTailSeqId = settlement.tailSeqId;
    return true;
  }

  bool consume(CpuReadyTape::ArenaGroupSettlement& settlement) noexcept {
    if (count == 0) {
      return false;
    }
    settlement = entries[head];
    if (!settlement.valid() || settlement.tailSeqId <= lastConsumedTailSeqId) {
      return false;
    }
    entries[head] = {};
    head = (head + 1u) % kCapacity;
    --count;
    lastConsumedTailSeqId = settlement.tailSeqId;
    return true;
  }

  const CpuReadyTape::ArenaGroupSettlement* front() const noexcept {
    return count == 0 ? nullptr : &entries[head];
  }
};

bool queueCompletionSourceExactlyEqual(
    const QueueCompletionSource& left,
    const QueueCompletionSource& right) noexcept;
bool queueCompletionSourceSpansExactlyEqual(
    std::span<const QueueCompletionSource> left,
    std::span<const QueueCompletionSource> right) noexcept;
// Compares only the locator-free projection represented by the shadow:
// dense seqId endpoints/count and tail-Present. Slot, source/storage locator,
// and command-range validation remain the old list/Tape authority's job.
bool encodedCompletionSpanShadowMatchesProjection(
    const std::optional<encoders::EncodedCompletionSpan>& shadow,
    std::span<const QueueCompletionSource> sources) noexcept;

// Ready selection stays small and stack-bounded. Encoded-unsubmitted work is
// independently bounded so physical Tape retirement does not turn the old
// 30-source residency ceiling into a grouping boundary.
inline constexpr size_t kMaxReadyPrefixSources = 32;
inline constexpr size_t kMaxEncodeSessionSources = 128;

struct EncodeSessionSourceList {
  std::array<QueueCompletionSource, kMaxEncodeSessionSources> entries{};
  size_t count = 0;

  bool canAppend(QueueCompletionSource source) const noexcept {
    if (!source.completionIdentityValid() ||
        count >= entries.size()) {
      return false;
    }
    if (count > 0) {
      const auto& previous = entries[count - 1u];
      if (previous.hasPresent || source.seqId != previous.seqId + 1u) {
        return false;
      }
    }
    return true;
  }

  bool append(QueueCompletionSource source) noexcept {
    if (!canAppend(source)) {
      return false;
    }
    entries[count++] = source;
    return true;
  }

  bool assign(std::span<const QueueCompletionSource> sources) noexcept {
    EncodeSessionSourceList next{};
    for (const auto& source : sources) {
      if (!next.append(source)) {
        return false;
      }
    }
    *this = next;
    return true;
  }

  bool replaceIdentity(const QueueCompletionSource& expected,
                       const QueueCompletionSource& replacement) noexcept {
    if (!expected.completionIdentityValid() ||
        !replacement.completionIdentityValid() ||
        expected.seqId != replacement.seqId) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      auto& entry = entries[i];
      if (entry.seqId == expected.seqId &&
          entry.source == expected.source &&
          entry.receipt == expected.receipt) {
        entry.source = replacement.source;
        entry.receipt = replacement.receipt;
        return true;
      }
      if (entry.seqId == replacement.seqId &&
          entry.source == replacement.source &&
          entry.receipt == replacement.receipt) {
        return true;
      }
    }
    return false;
  }

  void clear() noexcept {
    entries = {};
    count = 0;
  }
  bool empty() const noexcept { return count == 0; }
  size_t size() const noexcept { return count; }

  std::span<const QueueCompletionSource> span() const noexcept {
    return std::span<const QueueCompletionSource>(entries.data(), count);
  }

  const QueueCompletionSource* begin() const noexcept { return entries.data(); }
  const QueueCompletionSource* end() const noexcept {
    return entries.data() + count;
  }
};

struct ReadySlotSnapshot {
  size_t slotIndex = 0;
  u64 seqId = 0;
  CpuReadySourceMetadata metadata{};
  SourceSemanticSummary semantic{};
  bool hasPresent = false;
  size_t commandBegin = 0;
  size_t commandCount = 0;
  CpuReadySourceId sourceId{};
  CpuReadyStorageRef storage{};

  friend constexpr bool operator==(ReadySlotSnapshot,
                                   ReadySlotSnapshot) noexcept = default;
};

// Value-only state owned by the encode worker between queue events.  Keeping
// this separate from a resolved payload is deliberate: this object may be
// copied into planner scratch or a selected-parallel work item, while the
// payload itself must be reacquired through a synchronous borrow.
class WorkerOwnedSourceSnapshot final {
 public:
  WorkerOwnedSourceSnapshot() = default;
  explicit WorkerOwnedSourceSnapshot(const ReadySlotSnapshot& value) noexcept
      : value_(value) {}

  const ReadySlotSnapshot& value() const noexcept { return value_; }
  ReadySlotSnapshot copyValue() const noexcept { return value_; }
  bool valid() const noexcept {
    return value_.seqId != 0 && value_.metadata.valid() &&
           value_.sourceId.valid() && value_.storage.valid();
  }

 private:
  ReadySlotSnapshot value_{};
};

static_assert(std::is_trivially_copyable_v<WorkerOwnedSourceSnapshot>);

// Synchronous call-local resolution: either while the queue lock holds a Ready
// prefix stable for selection, or while a Represented Tape pin protects an
// encode call. Never store this in a lifecycle/session/partition/submission
// snapshot or callback.
struct ResolvedPublishedSource {
  CpuReadyTape::SourceRef source{};
  size_t slotIndex = 0;
  u64 seqId = 0;
  CpuReadySourceMetadata metadata{};
  SourceSemanticSummary semantic{};
  SourcePayloadView payload{};
  CpuReadySourceId sourceId{};
  CpuReadyStorageRef storage{};
  const ChunkSlot* slot = nullptr;
  bool hasPresent = false;
  size_t commandBegin = 0;
  size_t commandCount = 0;

  bool valid() const noexcept {
    return source.valid() && seqId != 0 && metadata.valid() &&
           source.id.generation != 0 && source.storage.generation != 0 &&
           semantic.sealed() && payload.valid();
  }
};

class QueueLifecycleController;
class GenerationQualifiedSourceBorrow;
class SynchronousSourcePayloadBorrow;
class SynchronousSourceBorrowBatch;

// Queue-owned liveness authority for synchronous source borrows. An epoch is
// active only while QueueLifecycleController is invoking the associated
// callback. Exact Tape source/storage generations are validated before unlock
// and again after relock; unlocked capability access reads only the immutable
// frozen record plus the atomic epoch. The locators remain pinned against the
// queue's reclaim path throughout that interval.
class SynchronousSourceBorrowWitness final {
 public:
  SynchronousSourceBorrowWitness(
      const SynchronousSourceBorrowWitness&) = delete;
  SynchronousSourceBorrowWitness& operator=(
      const SynchronousSourceBorrowWitness&) = delete;

 private:
  friend class QueueLifecycleController;
  friend class GenerationQualifiedSourceBorrow;
  friend class SynchronousSourcePayloadBorrow;
  friend class SynchronousSourceBorrowBatch;

  SynchronousSourceBorrowWitness() = default;

  std::uint64_t activate(
      CpuReadyTape* tape,
      std::span<const ResolvedPublishedSource> sources) noexcept;
  void deactivate(std::uint64_t epoch) noexcept;
  bool active(std::uint64_t epoch) const noexcept;
  bool validatesTapeLocked(std::uint64_t epoch,
                           const ResolvedPublishedSource& source,
                           CpuReadyTape::State expectedState) const noexcept;
  bool validatesFrozen(std::uint64_t epoch,
                       const ResolvedPublishedSource& source) const noexcept;
  std::span<const ResolvedPublishedSource> frozenSources(
      std::uint64_t epoch) const noexcept;
  bool invalidateFirstFrozenGenerationForTest() noexcept;
  bool pins(CpuReadyTape::SourceRef source) const noexcept;

  mutable std::mutex mutex_{};
  std::atomic<std::uint64_t> activeEpoch_{0};
  std::uint64_t nextEpoch_ = 1;
  CpuReadyTape* tape_ = nullptr;
  std::array<CpuReadyTape::SourceRef, kMaxReadyPrefixSources> pinnedSources_{};
  std::array<ResolvedPublishedSource, kMaxReadyPrefixSources> frozenSources_{};
  std::size_t pinnedSourceCount_ = 0;
};

static_assert(sizeof(ReadySlotSnapshot) <= 384,
              "Ready source capability must remain compact");
static_assert(sizeof(ResolvedPublishedSource) <= 448,
              "synchronous resolved source must remain compact");
static_assert(
    kMaxReadyPrefixSources *
            (sizeof(CpuReadyTape::ReadyEntry) +
             sizeof(ResolvedPublishedSource)) <=
        25u * 1024u,
        "Ready-prefix selector scratch must stay within its fixed stack budget");

// Noncopyable, epoch-qualified access to one source payload. Public callers
// receive only checked projections; the queue and concrete backend may unwrap
// the underlying view while the surrounding queue-owned batch epoch is live.
// Returned command/span projections are still borrowed C++ values: deliberate
// address retention cannot be made impossible by the type system and remains
// outside the synchronous callback contract.
class SynchronousSourcePayloadBorrow final {
 public:
  SynchronousSourcePayloadBorrow(const SynchronousSourcePayloadBorrow&) = delete;
  SynchronousSourcePayloadBorrow& operator=(
      const SynchronousSourcePayloadBorrow&) = delete;
  SynchronousSourcePayloadBorrow(SynchronousSourcePayloadBorrow&&) = delete;
  SynchronousSourcePayloadBorrow& operator=(
      SynchronousSourcePayloadBorrow&&) = delete;

  bool valid() const noexcept;
  bool isLegacy() const noexcept { return checkedView().isLegacy(); }
  bool isArena() const noexcept { return checkedView().isArena(); }
  std::size_t commandCount() const noexcept {
    return checkedView().commandCount();
  }
  bool commandsEmpty() const noexcept { return commandCount() == 0u; }
  std::size_t presentRecordCount() const noexcept {
    return checkedView().presentRecordCount();
  }
  bool drawOnlyCommandStream() const noexcept {
    return checkedView().drawOnlyCommandStream();
  }
  SourceCommandView commandAt(std::size_t index) const noexcept {
    return checkedView().commandAt(index);
  }

 private:
  friend class ::dxmt9::CommandQueue;
  friend class ::dxmt9::render::FrameGraphBackend;
  friend class GenerationQualifiedSourceBorrow;

  SynchronousSourcePayloadBorrow(
      const SynchronousSourceBorrowWitness& witness,
      std::uint64_t epoch,
      const ResolvedPublishedSource& resolved) noexcept
      : witness_(&witness),
        epoch_(epoch),
        resolved_(&resolved) {}

  SourcePayloadView checkedView() const noexcept;

  const SynchronousSourceBorrowWitness* witness_ = nullptr;
  std::uint64_t epoch_ = 0;
  const ResolvedPublishedSource* resolved_ = nullptr;
};

static_assert(!std::is_copy_constructible_v<SynchronousSourcePayloadBorrow>);
static_assert(!std::is_move_constructible_v<SynchronousSourcePayloadBorrow>);

// A generation-qualified, synchronous capability.  It is intentionally
// neither copyable nor movable, so a callback cannot place the capability in
// a completion record or worker closure by value.  The queue keeps the
// represented Tape source pinned until the callback returns; any cross-thread
// handoff must use WorkerOwnedSourceSnapshot and reacquire a fresh borrow.
class GenerationQualifiedSourceBorrow final {
 public:
  GenerationQualifiedSourceBorrow(const GenerationQualifiedSourceBorrow&) = delete;
  GenerationQualifiedSourceBorrow& operator=(
      const GenerationQualifiedSourceBorrow&) = delete;
  GenerationQualifiedSourceBorrow(GenerationQualifiedSourceBorrow&&) = delete;
  GenerationQualifiedSourceBorrow& operator=(GenerationQualifiedSourceBorrow&&) = delete;

  bool valid() const noexcept;
  const CpuReadyTape::SourceRef& source() const noexcept { return source_; }
  const CpuReadySourceMetadata& metadata() const noexcept { return metadata_; }
  const SourceSemanticSummary& semantic() const noexcept { return semantic_; }
  std::size_t slotIndex() const noexcept { return slotIndex_; }
  std::uint64_t seqId() const noexcept { return seqId_; }
  std::size_t commandBegin() const noexcept { return commandBegin_; }
  std::size_t commandCount() const noexcept { return commandCount_; }
  bool hasPresent() const noexcept { return hasPresent_; }

  // Checked synchronous payload access. The callback receives a noncopyable
  // capability rather than SourcePayloadView. C++ cannot prevent a hostile
  // caller from retaining an address obtained from a command/span projection;
  // that remains outside the contract, while normal capability access is
  // epoch-checked and the queue invalidates the epoch on batch return.
  template <typename Fn>
  bool visitPayload(Fn&& fn) const noexcept {
    if (!valid()) {
      return false;
    }
    const SynchronousSourcePayloadBorrow payload(
        *witness_, epoch_, *resolved_);
    if constexpr (std::is_same_v<
                      std::invoke_result_t<
                          Fn&, const SynchronousSourcePayloadBorrow&>,
                      bool>) {
      return std::invoke(fn, payload);
    } else {
      std::invoke(fn, payload);
      return true;
    }
  }

 private:
  friend class SynchronousSourceBorrowBatch;
  friend class QueueLifecycleController;

  explicit GenerationQualifiedSourceBorrow(
      const SynchronousSourceBorrowWitness& witness,
      std::uint64_t epoch,
      const ResolvedPublishedSource& resolved) noexcept
      : witness_(&witness),
        epoch_(epoch),
        resolved_(&resolved),
        source_(resolved.source),
        slotIndex_(resolved.slotIndex),
        seqId_(resolved.seqId),
        metadata_(resolved.metadata),
        semantic_(resolved.semantic),
        hasPresent_(resolved.hasPresent),
        commandBegin_(resolved.commandBegin),
        commandCount_(resolved.commandCount) {}

  const SynchronousSourceBorrowWitness* witness_ = nullptr;
  std::uint64_t epoch_ = 0;
  const ResolvedPublishedSource* resolved_ = nullptr;
  CpuReadyTape::SourceRef source_{};
  std::size_t slotIndex_ = 0;
  std::uint64_t seqId_ = 0;
  CpuReadySourceMetadata metadata_{};
  SourceSemanticSummary semantic_{};
  bool hasPresent_ = false;
  std::size_t commandBegin_ = 0;
  std::size_t commandCount_ = 0;
};

static_assert(!std::is_copy_constructible_v<GenerationQualifiedSourceBorrow>);
static_assert(!std::is_move_constructible_v<GenerationQualifiedSourceBorrow>);

// A call-local collection of generation-qualified borrows.  The backing
// records remain owned by the queue's selected-prefix scratch; this wrapper
// deliberately exposes no span, iterator, or element reference.  Consumers
// must process each borrow during visit(), so a backend cannot accidentally
// put a borrowed source view into a session, completion record, or async
// closure.  The queue keeps the represented sources pinned until the visit
// returns.
class SynchronousSourceBorrowBatch final {
 public:
  SynchronousSourceBorrowBatch(const SynchronousSourceBorrowBatch&) = delete;
  SynchronousSourceBorrowBatch& operator=(
      const SynchronousSourceBorrowBatch&) = delete;
  SynchronousSourceBorrowBatch(SynchronousSourceBorrowBatch&&) = delete;
  SynchronousSourceBorrowBatch& operator=(SynchronousSourceBorrowBatch&&) =
      delete;

  std::size_t size() const noexcept { return sources_.size(); }
  bool empty() const noexcept { return sources_.empty(); }
  bool live() const noexcept;

  template <typename Fn>
  bool visitAt(std::size_t index, Fn&& fn) const noexcept {
    if (!live() || index >= sources_.size()) {
      return false;
    }
    const GenerationQualifiedSourceBorrow borrow(
        *witness_, epoch_, sources_[index]);
    if (!borrow.valid()) {
      return false;
    }
    if constexpr (std::is_same_v<
                      std::invoke_result_t<Fn&, const GenerationQualifiedSourceBorrow&>,
                      bool>) {
      return std::invoke(fn, borrow);
    } else {
      std::invoke(fn, borrow);
      return true;
    }
  }

  template <typename Fn>
  bool visit(Fn&& fn) const noexcept {
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (!live()) {
        return false;
      }
      const GenerationQualifiedSourceBorrow borrow(
          *witness_, epoch_, sources_[i]);
      if (!borrow.valid()) {
        return false;
      }
      if constexpr (std::is_same_v<
                        std::invoke_result_t<Fn&, const GenerationQualifiedSourceBorrow&,
                                             std::size_t>,
                        bool>) {
        if (!std::invoke(fn, borrow, i)) {
          return false;
        }
      } else {
        std::invoke(fn, borrow, i);
      }
    }
    return true;
  }

 private:
  friend class QueueLifecycleController;

  SynchronousSourceBorrowBatch(
      const SynchronousSourceBorrowWitness& witness,
      std::uint64_t epoch,
      std::span<const ResolvedPublishedSource> sources) noexcept
      : witness_(&witness),
        epoch_(epoch),
        sources_(sources) {}

  const SynchronousSourceBorrowWitness* witness_ = nullptr;
  std::uint64_t epoch_ = 0;
  std::span<const ResolvedPublishedSource> sources_{};
};

static_assert(!std::is_copy_constructible_v<SynchronousSourceBorrowBatch>);
static_assert(!std::is_move_constructible_v<SynchronousSourceBorrowBatch>);

// Stable, pointer-free attribution for one logical command in a published
// source. The Tape source/storage generations disambiguate recycled control
// slots; slotIndex is diagnostic only and completion remains source-granular.
struct PublishedCommandRef {
  CpuReadyTape::SourceRef source{};
  u64 seqId = 0;
  u32 slotIndex = std::numeric_limits<u32>::max();
  u32 commandIndex = std::numeric_limits<u32>::max();

  constexpr bool valid() const noexcept {
    return source.valid() && seqId != 0 &&
           slotIndex != std::numeric_limits<u32>::max() &&
           commandIndex != std::numeric_limits<u32>::max();
  }

  friend constexpr bool operator==(PublishedCommandRef,
                                   PublishedCommandRef) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<PublishedCommandRef>);
static_assert(std::is_standard_layout_v<PublishedCommandRef>);

QueueCompletionSource completionSourceForReadySlot(
    const ReadySlotSnapshot& snapshot) noexcept;

// Non-owning call-local thunk for hot queue selectors. It never allocates;
// callers must not retain it beyond the invoking queue operation.
template <typename Signature>
class QueueBorrowCallbackRef;

template <typename Result, typename... Args>
class QueueBorrowCallbackRef<Result(Args...)> final {
 public:
  QueueBorrowCallbackRef() = default;

  template <typename Fn>
    requires (!std::is_same_v<std::remove_cvref_t<Fn>, QueueBorrowCallbackRef> &&
              std::is_invocable_r_v<Result, Fn&, Args...>)
  QueueBorrowCallbackRef(Fn&& fn) noexcept
      : context_(std::addressof(fn)),
        invoke_([](void* context, Args... args) -> Result {
          using Callable = std::remove_reference_t<Fn>;
          return std::invoke(*static_cast<Callable*>(context),
                             std::forward<Args>(args)...);
        }) {}

  explicit operator bool() const noexcept { return invoke_ != nullptr; }
  Result operator()(Args... args) const {
    return invoke_(context_, std::forward<Args>(args)...);
  }

 private:
  void* context_ = nullptr;
  Result (*invoke_)(void*, Args...) = nullptr;
};

using ReadySlotBatchAppendPredicate =
    QueueBorrowCallbackRef<bool(
        std::size_t selectedCount,
        const GenerationQualifiedSourceBorrow& candidate)>;
using ReadySlotBatchPrefixSelector =
    QueueBorrowCallbackRef<
        size_t(const SynchronousSourceBorrowBatch& candidates)>;

struct QueueSubmissionRecord {
  struct RenderEncoderGpuSample {
    u32 startIndex = 0;
    u32 endIndex = 0;
    RenderEncoderGpuPassType passType = RenderEncoderGpuPassType::Unknown;
    u64 seqId = 0;
    u32 slotIndex = 0;
    u32 commandIndex = 0;
    encoders::EncodedCommandId sourceCommand{};
    u64 rtHandle = 0;
    u64 depthHandle = 0;
    u64 psoHandle = 0;
  };

  // RAII-owned command buffer for the tail of this chunk's Metal command
  // buffer chain. encodeChunk may commit earlier sub-CBs internally; the
  // finish/completion pipeline commits and waits only this tail CB, relying
  // on Metal same-queue in-order execution to make tail completion imply all
  // earlier sub-CBs in this chunk have completed.
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
  // CPU-only lifecycle specs may exercise submit preflight without creating an
  // Objective-C command buffer. Production must leave this false.
  bool testOnlyAllowNullCommandBuffer = false;
  // Total command buffers in the chunk chain, including the tail above.
  // 1 means the public record is the whole chunk; >1 means earlier sub-CBs
  // were already committed during encodeChunk.
  u64 commandBufferChainLength = 1;
  WMT::Device metalCaptureDevice{};
  std::optional<metalcapture::MetalCaptureRequest> metalCapture{};
  // True when MTLCaptureManager.startCapture was already issued at
  // chunk-begin (in encodeChunk) so every encoder/draw call is in scope.
  // commitCommandBuffer skips the legacy start in that case and only
  // issues stopCapture after commit.
  bool metalCaptureAlreadyStarted = false;
  size_t slotIndex = 0;
  u64 seqId = 0;
  // Empty means the legacy one-slot submission: (slotIndex, seqId,
  // diagnostics.hasPresent). EncodeSession/pass-streaming and multi-source
  // paths fill this with every source slot completed by the same tail command
  // buffer, in strict seqId order, without heap allocation.
  EncodeSessionSourceList fixedCompletionSources{};
  std::optional<encoders::EncodedCompletionSpan> completionSpanShadow{};
  std::span<const QueueCompletionSource> explicitCompletionSourceSpan()
      const noexcept {
    return fixedCompletionSources.span();
  }
  bool assignFixedCompletionSources(
      std::span<const QueueCompletionSource> sources) noexcept;
  void clearFixedCompletionSources() noexcept {
    fixedCompletionSources.clear();
    completionSpanShadow.reset();
  }
  bool completionSpanShadowMatchesSources() const noexcept {
    return encodedCompletionSpanShadowMatchesProjection(
        completionSpanShadow, explicitCompletionSourceSpan());
  }
  CommandBufferDiagnostics diagnostics{};
  const char* context = "queue";
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
  std::vector<RenderEncoderGpuSample> renderEncoderGpuSamples{};
  std::vector<std::function<void()>> postCommitCallbacks;
  std::vector<std::function<void()>> completionCallbacks;
  std::vector<std::shared_ptr<void>> retainedPayloads;
};

// True only when the record's non-empty explicit list, the queue-owned
// pending list, and the session-owned list are the same complete FIFO source
// sequence. Callers may use this before the first Metal effect to select an
// empty canonical representation; it performs no mutation itself.
bool hasExactRedundantFixedCompletionSources(
    const QueueSubmissionRecord& record,
    std::span<const QueueCompletionSource> pendingSources,
    std::span<const QueueCompletionSource> sessionSources) noexcept;

// A standalone represented source must complete through exactly its own Tape
// locator and command range. Empty records receive that singleton; non-empty
// records are accepted only when they are already the exact same singleton.
bool assignOrValidateSingleCompletionSource(
    QueueSubmissionRecord& record,
    const ReadySlotSnapshot& source) noexcept;

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations);
CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                          size_t slotIndex,
                                          const ChunkSlot& slot,
                                          const std::function<u32(Handle)>& resolveSurfaceFlags);
CommandBufferDiagnostics mergeCommandBufferDiagnostics(
    CommandBufferDiagnostics aggregate,
    const CommandBufferDiagnostics& source) noexcept;
// Fold metadata from an earlier encoded fragment carrier into the newest tail
// without interpreting either record as a completion-source boundary. Record
// identity and fixedCompletionSources remain those of newTail. Different live
// CB handles are accepted only when newTail's fragment chain proves that it
// committed the injected old tail and returned a successor command buffer.
// Deterministic preflight rejection leaves both records unchanged.
bool foldEncodedSessionFragmentCarrier(
    QueueSubmissionRecord& newTail,
    QueueSubmissionRecord& oldCarrier,
    obj_handle_t injectedCommandBuffer = NULL_OBJECT_HANDLE);
bool mergeEncodedPendingTailSubmission(
    QueueSubmissionRecord& tail,
    QueueSubmissionRecord& encodedHead,
    std::span<const QueueCompletionSource> encodedHeadSources,
    QueueCompletionSource tailSource,
    bool encodedHeadTailAlreadyCommitted = false,
    EncodeSessionSourceList* mergedSourcesOut = nullptr);
Handle selectPresentSourceHandle(const SwapDesc& desc, Handle currentBackBuffer) noexcept;
QueueTraceSnapshot makeQueueTraceSnapshot(const QueueTraceState& state);
void appendCompletionSourcesToQueues(
    std::deque<u64>& completedSeqQueue,
    std::deque<u64>* completedPresentSeqQueue,
    u64 completedSeqId,
    std::span<const QueueCompletionSource> sources);

template <typename CommandContainer, typename ObservationMapper>
CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        const CommandContainer& commands,
                                        ObservationMapper&& mapObservation) {
  std::vector<ChunkObservation> observations;
  observations.reserve(commands.size());
  for (const auto& command : commands) {
    observations.push_back(std::invoke(std::forward<ObservationMapper>(mapObservation), command));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

template <typename SlotContainer, typename SlotMapper>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots,
                                          SlotMapper&& mapSlot) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    auto mapped = std::invoke(std::forward<SlotMapper>(mapSlot), i, slots[i]);
    if (mapped.has_value()) {
      state.activeSlots.push_back(*mapped);
    }
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.state == ChunkSlotControl::State::Free) {
      continue;
    }
    state.activeSlots.push_back(ActiveSlotInfo{
        .index = i,
        .state = static_cast<QueueSlotState>(static_cast<int>(slot.state)),
        .seqId = slot.seqId,
        .commandCount = slot.commandCount(),
    });
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer, typename SlotMapper>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     SlotMapper&& mapSlot,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots,
                             std::forward<SlotMapper>(mapSlot)),
      extra);
}

template <typename SlotContainer>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots),
      extra);
}

bool queueTraceEnabled();
const char* queueTraceFilePath();
u64 queueTraceFromSeq();

void emitQueueTraceLine(const std::string& line);
void emitTextureTraceLine(const std::string& line);

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input);
bool shouldTraceQueue(const QueueTraceSnapshot& snapshot);
std::string formatActiveSlots(const QueueTraceSnapshot& snapshot);
void traceQueueEvent(const char* event, const QueueTraceSnapshot& snapshot, const char* extra = nullptr);
void traceLifecycleEvent(QueueLifecycleEvent event,
                         std::optional<size_t> slotIndex,
                         u64 eventSeqId,
                         std::optional<size_t> writingSlot,
                         size_t writeIndex,
                         size_t readyCount,
                         size_t completedQueueCount,
                         size_t inflightCount,
                         u64 completedSeqId,
                         u64 lastCommittedSeqId,
                         std::span<const ChunkSlotControl> slots,
                         const char* extra = nullptr);
void traceQueueSlotsEvent(const char* event,
                          std::optional<size_t> slotIndex,
                          u64 eventSeqId,
                          std::optional<size_t> writingSlot,
                          size_t writeIndex,
                          size_t readyCount,
                          size_t completedQueueCount,
                          size_t inflightCount,
                          u64 completedSeqId,
                          u64 lastCommittedSeqId,
                          std::span<const ChunkSlotControl> slots,
                          const char* extra = nullptr);

/*
 * TLA+: QueueLifecycleRefinement
 *
 * Variable mapping:
 *   writingSlot               -> *SubmissionBinding::writingSlot
 *   writeIndex                -> *SubmissionBinding::writeIndex
 *   nextSeqId                 -> *SubmissionBinding::nextSeqId
 *   readySlots                -> SubmissionBinding::cpuReadyTape Ready FIFO
 *   completedSeqQueue         -> *SubmissionBinding::completedSeqQueue
 *   pendingCompletion         -> pendingCompletion_
 *   inflightCount             -> *SubmissionBinding::inflightCount
 *   completedSeqId            -> *SubmissionBinding::completedSeqId
 *   lastCommittedSeqId        -> *SubmissionBinding::lastCommittedSeqId
 *   stop                      -> *SubmissionBinding::stop
 *   slotState[s]              -> SubmissionBinding::slots[s].state
 *   slotSeqId[s]              -> SubmissionBinding::slots[s].seqId
 *   slotHasCommands[s]        -> !SubmissionBinding::slots[s].commandsEmpty()
 *
 * TLA+: PresentFrameLatency
 *
 * Variable mapping:
 *   presentCompletedSeqId     -> *SubmissionBinding::presentCompletedSeqId
 *   present completion queue  -> *SubmissionBinding::completedPresentSeqQueue
 *   completedPresentOrdinal   -> *SubmissionBinding::completedPresentOrdinal (ordinal variant)
 *
 * Debug assertions in assertQueueLifecycleInvariants() and
 * assertPendingCompletionInvariantsLocked() are the executable binding for
 * the safety invariants in both modules.
 */
class QueueLifecycleController {
 public:
  // Failure-only provenance for lifecycle poison.  The source-location
  // strings point at static compiler literals; publication is one-shot and
  // never participates in normal queue payloads or transition records.
  struct PoisonOriginSnapshot {
    const char* file = nullptr;
    const char* function = nullptr;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    bool valid() const noexcept { return file != nullptr && line != 0; }
  };

  struct SubmissionBinding {
    std::optional<size_t>* writingSlot = nullptr;
    size_t* writeIndex = nullptr;
    // Atomic only so the mark ticket can be read without `mutex` (design
    // T2a/T2a'); every write below still happens with `mutex` held, so the
    // relaxed loads inside this controller are exact. See the memory-order
    // argument on `CommandQueue::nextSeqId_`.
    std::atomic<u64>* nextSeqId = nullptr;
    std::deque<u64>* completedSeqQueue = nullptr;
    std::deque<u64>* completedPresentSeqQueue = nullptr;
    ArenaGroupSettlementLedger*
        completedArenaGroupSettlements = nullptr;
    size_t* inflightCount = nullptr;
    // Atomic only so the map DISCARD fast path can read the GPU watermark
    // without `mutex` (design T2c). This controller is the SOLE writer
    // (`drainCompletedSequence`) and always writes with `mutex` held, so the
    // relaxed loads inside it are exact and the release store is what the
    // producer's acquire load pairs with. See the memory-order argument on
    // `CommandQueue::completedSeqId_`.
    std::atomic<u64>* completedSeqId = nullptr;
    u64* presentCompletedSeqId = nullptr;
    // Commit-replay offload ordinal (TLA+: PresentFrameLatency ordinal
    // variant). Incremented once per present retired in drainCompletedSequence,
    // alongside presentCompletedSeqId; nullptr in bindings that don't wire
    // the offload path (e.g. the completion-sources spec).
    u64* completedPresentOrdinal = nullptr;
    u64* lastCommittedSeqId = nullptr;
    std::span<ChunkSlotControl> slots;
    CpuReadyTape* cpuReadyTape = nullptr;
    std::mutex* mutex = nullptr;
    std::condition_variable* writeCv = nullptr;
    std::condition_variable* encodeCv = nullptr;
    std::condition_variable* finishCv = nullptr;
    std::condition_variable* presentCompletedCv = nullptr;
    std::condition_variable* presentDequeuedCv = nullptr;
    std::condition_variable* sessionReleaseCv = nullptr;
    bool* stop = nullptr;
    metalhud::SubmissionDiagnosticsController* submissionDiagnostics = nullptr;
    SchedulingProgressWatchdog* schedulingProgressWatchdog = nullptr;
    std::function<u32(Handle)> resolveSurfaceFlags;
    // Nullable diagnostic sink.  The queue owns the observer; the controller
    // only forwards owner evidence and never allocates on the disabled path.
    ::dxmt9::queue::PipelineLifecycleObserverSink pipelineLifecycleObserver{};
  };

  void bindTrackedSubmissionState(SubmissionBinding binding);
  // Record an arena/source admission before the Tape entry becomes Ready.
  // The caller supplies the immutable admission identity because Tape quite
  // intentionally does not expose metadata while the entry is Writing.
  void recordPipelineSourceArrival(CpuReadyTape::PayloadKind payloadKind,
                                   CpuReadyTape::SourceRef source,
                                   CpuReadyAdmissionIdentity identity,
                                   std::size_t ownedBytes) const noexcept;
  // R-BACK-2.65 / SessionCapacityLease: publish every transition that can
  // reduce the physical residency excluded from a new session lease. Callers
  // hold the queue scheduling mutex, so the generation and the capacity
  // snapshot used by the waiter change in one serialized transaction.
  void noteCpuReadyCapacityProgress() noexcept;
  std::uint64_t cpuReadyCapacityProgressGeneration() const noexcept {
    return cpuReadyCapacityProgressGeneration_;
  }
  std::uint64_t gpuOutstandingCompletionSourceCount() const noexcept {
    return gpuOutstandingCompletionSourceCount_;
  }
  // TLA+: WriterAcquire, WriterWaitBegin, WriterWaitEnd.
  bool ensureWriterSlot(std::unique_lock<std::mutex>& lock, size_t inflightLimit);
  // TLA+: Present enqueue + CommitPublish for a present-bearing chunk.
  bool presentAndCommit(std::unique_lock<std::mutex>& lock,
                        size_t inflightLimit,
                        const SwapDesc& present,
                        Handle sourceHandle,
                        const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: CommitPublish followed by waitForSequence(lastCommittedSeqId).
  void flushAndWait(std::unique_lock<std::mutex>& lock,
                    size_t inflightLimit,
                    const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: CommitEmpty or CommitPublish.
  bool commitCurrentChunk(std::unique_lock<std::mutex>& lock,
                          size_t inflightLimit,
                          const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: EncodeDequeue.
  bool dequeueReadySlot(std::unique_lock<std::mutex>& lock,
                        ReadySlotSnapshot& out);
  // TLA+: EncodeDequeue for one or more consecutive ready slots. The caller
  // owns `out`; no heap allocation is performed inside the queue primitive.
  size_t dequeueReadySlotBatch(std::unique_lock<std::mutex>& lock,
                               std::span<ReadySlotSnapshot> out,
                               const ReadySlotBatchAppendPredicate& canAppend = {});
  // TLA+: EncodeDequeue for a caller-selected ready-slot prefix. The selector
  // must only return a FIFO prefix length. Returning zero falls back to the
  // legacy single-source dequeue so incomplete multi-source patterns cannot
  // be consumed accidentally.
  size_t dequeueReadySlotBatchPrefix(std::unique_lock<std::mutex>& lock,
                                     std::span<ReadySlotSnapshot> out,
                                     const ReadySlotBatchPrefixSelector& selectPrefix);
  // Transactional form of EncodeDequeue. A selected FIFO prefix leaves Ready
  // visibility but keeps its control slots Pending until exact semantic
  // preflight commits it. Returning zero leaves both Tape and controls intact;
  // unlike dequeueReadySlotBatchPrefix, selector rejection does not fall back
  // to one source.
  size_t reserveReadySlotBatchPrefix(
      std::unique_lock<std::mutex>& lock,
      std::span<ReadySlotSnapshot> out,
      const ReadySlotBatchPrefixSelector& selectPrefix);
  ResolvedPublishedSource resolveTentativeSource(
      std::unique_lock<std::mutex>& lock,
      const ReadySlotSnapshot& source) const noexcept;
  bool commitReservedReadySlotBatch(
      std::unique_lock<std::mutex>& lock,
      std::span<const ReadySlotSnapshot> sources);
  bool restoreReservedReadySlotBatch(
      std::unique_lock<std::mutex>& lock,
      std::span<const ReadySlotSnapshot> sources);
  ResolvedPublishedSource resolveRepresentedSource(
      const ReadySlotSnapshot& source) const noexcept;
  // Issue a non-forgeable callback-scoped batch after validating every source
  // against the queue-owned Tape state. The batch constructor is private;
  // these are the only backend-facing issuance paths.
  template <typename Visitor>
  bool visitTentativeSourceBorrows(
      std::unique_lock<std::mutex>& lock,
      std::span<const ResolvedPublishedSource> sources,
      Visitor&& visitor) {
    return visitSourceBorrows(
        lock, sources, CpuReadyTape::State::TentativeRepresented,
        std::forward<Visitor>(visitor));
  }
  template <typename Visitor>
  bool visitRepresentedSourceBorrows(
      std::unique_lock<std::mutex>& lock,
      std::span<const ResolvedPublishedSource> sources,
      Visitor&& visitor) {
    return visitSourceBorrows(lock, sources, CpuReadyTape::State::Represented,
                              std::forward<Visitor>(visitor));
  }
  // Native-only seam for the post-callback validation negative. It corrupts
  // the frozen expected generation, never the queue-owned Tape, and therefore
  // must be consumed by the active visit's locked post-validation.
  bool invalidateActiveBorrowGenerationForTest(
      std::unique_lock<std::mutex>& lock) noexcept {
    return lock.owns_lock() && lock.mutex() == submissionBinding_.mutex &&
        sourceBorrowWitness_.invalidateFirstFrozenGenerationForTest();
  }
  // Retire one fully encoded source while retaining locator-free completion
  // authority. The caller owns the queue scheduling lock; Arena destruction
  // and Legacy payload clearing run outside it, then the transaction relocks
  // before generation/page/control release and writer notification.
  PostEncodeReceiptResult retireEncodedSourcePayload(
      std::unique_lock<std::mutex>& lock,
      QueueCompletionSource& source);
  // Fail-stop a structurally inconsistent Tape/source resolution. Locked
  // callers already own SubmissionBinding::mutex; completion-thread callers
  // must use the unlocked entry point so Tape/admission/stop mutation remains
  // serialized by the scheduling owner.
  void poisonTapeFailureLocked(
      std::source_location location = std::source_location::current()) noexcept;
  void poisonTapeFailure(
      std::source_location location = std::source_location::current()) noexcept;
  PoisonOriginSnapshot firstPoisonOrigin() const noexcept {
    if (!firstPoisonOriginPublished_.load(std::memory_order_acquire)) {
      return {};
    }
    return {
        .file = firstPoisonOriginFile_.load(std::memory_order_relaxed),
        .function = firstPoisonOriginFunction_.load(std::memory_order_relaxed),
        .line = firstPoisonOriginLine_.load(std::memory_order_relaxed),
        .column = firstPoisonOriginColumn_.load(std::memory_order_relaxed),
    };
  }
  // Encoded-head retention for pending session tails. Sources must already be
  // dequeued into Encoding state; this records their completion identity
  // without making them ready-visible or GPU-complete.
  size_t retainEncodedSourcesForPendingTail(std::unique_lock<std::mutex>& lock,
                                            std::span<const ReadySlotSnapshot> sources,
                                            std::span<QueueCompletionSource> out);
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  bool runEncodeIteration(
      std::unique_lock<std::mutex>& lock,
      const std::function<std::optional<QueueSubmissionRecord>(
          const WorkerOwnedSourceSnapshot&,
          const GenerationQualifiedSourceBorrow&)>& encodeFn,
      const std::function<void(u64)>& onInlineComplete = {});
  // TLA+: present-bearing metadata append before CommitPublish.
  bool appendPresentCommand(const SwapDesc& present, Handle sourceHandle);
  // TLA+: EncodeSubmitToGpu.
  bool submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                          size_t slotIndex,
                          u64 seqId,
                          const char* context = "queue");
  // TLA+: EncodeSubmitToGpu for an externally prepared tail submission.
  // Used by split encode carriers once they have assembled the final
  // fixed completion-source chain.
  bool submitEncodedSubmission(std::unique_lock<std::mutex>& lock,
                               QueueSubmissionRecord& record);
  // TLA+: EncodeCompleteInline.
  bool completeInlineChunk(std::unique_lock<std::mutex>& lock,
                           size_t slotIndex,
                           u64 seqId);
  // TLA+: FinishDequeue, and PresentComplete for eligible present seq IDs.
  bool drainCompletedSequence(std::unique_lock<std::mutex>& lock, u64& seqId);
  // Consume value-owned SegmentSerial event settlement only once its tail has
  // crossed the queue completion waterline.
  bool drainCompletedArenaGroupSettlementsLocked(u64 completedSeqId) noexcept;
  bool hasCompletedArenaGroupSettlement(
      u64 rawOrdinal, u64 buildGeneration, u64 firstSourceOrdinal,
      u64 tailSeqId, std::uint32_t sourceCount) const noexcept;
  // TLA+: FinishDequeue followed by ReclaimFree.
  bool runFinishIteration(std::unique_lock<std::mutex>& lock,
                          const std::function<void(u64)>& onAfterFinish = {});
  // Reclaim every completed FIFO owner at or below the published completion
  // waterline. A valid SegmentSerial head may remain resident until its tail
  // completes; stale, corrupt, or skipped non-group heads still fail-stop.
  bool reclaimCompletedTapeHeadsThrough(std::unique_lock<std::mutex>& lock,
                                        u64 completedSeqId);
  // TLA+: ReclaimFree.
  bool reclaimCompletedTapeHead(std::unique_lock<std::mutex>& lock, u64 seqId);
  // TLA+: BeginWaitForSequence / EndWaitForSequence.
  void waitForSequence(std::unique_lock<std::mutex>& lock, u64 targetSeqId);
  // Queue-local observation used by scheduling diagnostics to determine
  // whether the completion thread is waiting. This does not participate in
  // ordering or lifetime decisions.
  bool completionWaitActive();
  // Queue-local observation for producer-side waits such as resource Lock/Map.
  // A pending session must be allowed to submit without the final Present tail
  // when the producer is blocked on a sequence it owns.
  bool producerSequenceWaitActive();
  // Exact highest ordered fence currently owned by producer-side sequence
  // waits. Zero means no producer wait is active.
  u64 producerSequenceWaitTargetSeqId();
  void waitForProducerSequenceWaitTargetForTest(u64 targetSeqId);
  // Queue-local observation for a compatibility writer blocked either on a
  // free control/Tape reservation or on the GPU-inflight publication cap.
  // This is a diagnostic/wakeup signal only. The Tape-gated session lane must
  // not derive a release fence or submission boundary from live pressure.
  bool producerWriterPressureActive();
  // Diagnostic stage probes from the last no-enqueue completion wait end to
  // producer-side commit_chunk milestones.
  void recordCompletionWaitCommitChunkEntry();
  void recordCompletionWaitCommitChunkReplayStart();
  void recordCompletionWaitCommitChunkReplayEnd(std::uint64_t replayNanoseconds);
  void recordNoEnqueueWaitGapToCommitChunkEntry();
  CpuReadySupplyObservationToken recordCpuReadySupplyReplayEntry(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId,
      CpuReadySupplyObservationToken attemptToken = {});
  void cancelCpuReadySupplyReplayEntry(
      CpuReadyTape::PayloadKind sourceClass,
      CpuReadySupplyObservationToken attemptToken);
  bool cpuReadySupplyReplayEntryPendingForTest(
      CpuReadyTape::PayloadKind sourceClass,
      CpuReadySupplyObservationToken attemptToken);
  void recordCpuReadySupplyPublished(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId);
  void recordCpuReadySupplyDequeued(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId);
  void recordNoEnqueueWaitGapToCommitChunkReplayStart();
  void recordNoEnqueueWaitGapToCommitChunkReplayEnd();
  void recordNoEnqueueCommitChunkReplayCpuBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitChunkRecordShapeBeforePublish(
      const NoEnqueueCommitChunkRecordShape& shape);
  void recordNoEnqueueFirstPublishSlotShapeBeforePublish(
      const NoEnqueueFirstPublishSlotShape& shape);

 private:
  template <typename Visitor>
  bool visitSourceBorrows(
      std::unique_lock<std::mutex>& lock,
      std::span<const ResolvedPublishedSource> sources,
      CpuReadyTape::State expectedState,
      Visitor&& visitor) {
    static_assert(std::is_nothrow_invocable_r_v<
                  bool, Visitor&, const SynchronousSourceBorrowBatch&>);
    if (!lock.owns_lock() || lock.mutex() != submissionBinding_.mutex ||
        !submissionBinding_.cpuReadyTape ||
        sources.size() > kMaxReadyPrefixSources) {
      return false;
    }
    const std::uint64_t epoch = sourceBorrowWitness_.activate(
        submissionBinding_.cpuReadyTape, sources);
    if (epoch == 0u) {
      return false;
    }
    struct EpochGuard {
      std::unique_lock<std::mutex>& lock;
      SynchronousSourceBorrowWitness& witness;
      std::uint64_t epoch;
      ~EpochGuard() {
        // A callback may release the scheduling mutex only for its bounded
        // backend work. Restore queue ownership before removing the pin so no
        // reclaim transition can fit between deactivation and reacquisition.
        if (!lock.owns_lock()) {
          lock.lock();
        }
        witness.deactivate(epoch);
      }
    } guard{lock, sourceBorrowWitness_, epoch};

    const auto frozenSources = sourceBorrowWitness_.frozenSources(epoch);
    if (frozenSources.size() != sources.size()) {
      return false;
    }
    for (const auto& source : frozenSources) {
      if (!sourceBorrowWitness_.validatesTapeLocked(
              epoch, source, expectedState)) {
        return false;
      }
    }
    const SynchronousSourceBorrowBatch batch(
        sourceBorrowWitness_, epoch, frozenSources);
    const bool accepted = std::invoke(visitor, batch);
    if (!lock.owns_lock()) {
      lock.lock();
    }
    for (const auto& source : frozenSources) {
      if (!sourceBorrowWitness_.validatesTapeLocked(
              epoch, source, expectedState)) {
        poisonTapeFailureLocked();
        return false;
      }
    }
    return accepted;
  }
  QueueControllerState currentState() const;
  QueueLifecycleEvent classifyTransition(const QueueTransitionRecord& record) const;
#ifndef NDEBUG
  void assertQueueLifecycleInvariants(size_t inflightLimit = 0) const;
  void assertPendingCompletionInvariantsLocked() const;
#endif
  CommandBufferDiagnostics summarizeSubmission(
      u64 seqId,
      size_t slotIndex) const;
  CommandBufferDiagnostics summarizeSubmissionSources(
      const QueueSubmissionRecord& record,
      std::span<const QueueCompletionSource> sources) const;
  void recordNoEnqueueWaitGapToCommitPublish();
  void recordNoEnqueueCommitPublishWaitBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitPublishOnBeforePublishCpu(
      std::uint64_t nanoseconds);
  void recordNoEnqueueWaitGapToEncodeDequeue();
  void recordNoEnqueueWaitGapToCommandBufferCommit();
  void observeTransition(const QueueTransitionRecord& record) const;
  void observePipelineOwnerTransition(const QueueTransitionRecord& record,
                                      QueueLifecycleEvent event) const noexcept;
  void observePipelinePoisonStop() const noexcept;
  void enqueuePresent(size_t slotIndex,
                      u64 eventSeqId,
                      const SwapDesc& present,
                      Handle sourceHandle,
                      const std::function<void()>& mutate = {});
  void observeWriterWait(size_t slotIndex, u64 eventSeqId, size_t inflightLimit);
  void acquireWriterSlot(size_t slotIndex,
                         u64 eventSeqId,
                         size_t inflightLimit,
                         const std::function<void()>& mutate = {});
  void commitEmpty(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void observeCommitWait(size_t slotIndex, u64 eventSeqId, size_t inflightLimit);
  void commitPublish(size_t slotIndex,
                     u64 eventSeqId,
                     size_t inflightLimit,
                     const std::function<void()>& mutate = {});
  void encodeDequeue(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void finishInline(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void finishDequeue(u64 eventSeqId, const std::function<void()>& mutate = {});
  void reclaimFree(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void observeWaitForSequence(u64 targetSeqId);
  void notePresentEnqueue(const QueueControllerState& state,
                          size_t slotIndex,
                          u64 eventSeqId,
                          const SwapDesc& present,
                          Handle sourceHandle) const;
  void noteWriterWaitBeginIfNeeded(const QueueControllerState& state,
                                   size_t slotIndex,
                                   u64 eventSeqId,
                                   size_t inflightLimit) const;
  void noteWriterWaitEnd(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteWriterAcquire(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteCommitEmpty(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteCommitWaitBeginIfNeeded(const QueueControllerState& state,
                                   size_t slotIndex,
                                   u64 eventSeqId,
                                   size_t inflightLimit) const;
  void noteCommitWaitEnd(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteCommitPublish(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteEncodeDequeue(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteEncodeCommit(const QueueControllerState& state,
                        size_t slotIndex,
                        u64 eventSeqId) const;
  void noteGpuComplete(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteFinishInline(const QueueControllerState& state,
                        size_t slotIndex,
                        u64 eventSeqId) const;
  void noteFinishDequeue(const QueueControllerState& state,
                         u64 eventSeqId) const;
  void noteReclaimFree(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteWaitSeqBeginIfNeeded(const QueueControllerState& state,
                                u64 targetSeqId) const;
  void noteWaitSeqEnd(const QueueControllerState& state,
                      u64 targetSeqId) const;
  void transition(QueueTransitionRecord record, const std::function<void()>& mutate = {});
  bool enqueueSubmission(QueueSubmissionRecord& record);
  bool submit(QueueSubmissionRecord& record);

  SubmissionBinding submissionBinding_{};
  SynchronousSourceBorrowWitness sourceBorrowWitness_{};
  std::array<ReadySlotSnapshot, kMaxReadyPrefixSources>
      tentativeReadyPrefix_{};
  size_t tentativeReadyPrefixCount_ = 0;
  std::uint64_t cpuReadyCapacityProgressGeneration_ = 0;
  PostEncodeCompletionLedger postEncodeCompletionLedger_{};
  ArenaGroupSettlementLedger completedArenaGroupSettlements_{};
  std::uint64_t completedEventSettlementCount_ = 0;
  std::uint64_t completedEventTailSeqId_ = 0;
  std::optional<CpuReadyTape::ArenaGroupSettlement>
      lastCompletedEventSettlement_{};
  std::array<CpuReadyTape::ArenaGroupSettlement,
             ArenaGroupSettlementLedger::kCapacity>
      completedEventSettlementHistory_{};
  std::size_t completedEventSettlementHistoryHead_ = 0;
  std::size_t completedEventSettlementHistoryCount_ = 0;
  std::uint64_t gpuOutstandingCompletionSourceCount_ = 0;
  // These fields are touched only when fail-stop poison is requested.  Keep
  // them outside SubmissionBinding and all transition/payload structures so
  // normal queue hot paths retain their existing shape.
  std::atomic<bool> firstPoisonOriginClaimed_{false};
  std::atomic<bool> firstPoisonOriginPublished_{false};
  std::atomic<const char*> firstPoisonOriginFile_{nullptr};
  std::atomic<const char*> firstPoisonOriginFunction_{nullptr};
  std::atomic<std::uint32_t> firstPoisonOriginLine_{0};
  std::atomic<std::uint32_t> firstPoisonOriginColumn_{0};

 public:
  // Records that have been committed to Metal and are awaiting GPU completion.
  // The owning MetalBackendDevice runs a dedicated completion-watcher thread
  // that pops from this queue, calls waitUntilCompleted() on each
  // commandBuffer, and then drives the downstream completion pipeline
  // (diagnostics + completedSeqQueue + transitions).
  struct PendingCompletion {
    WMT::Reference<WMT::CommandBuffer> commandBuffer;
    CommandBufferDiagnostics diagnostics{};
    std::string contextValue{};
    size_t slotIndex = 0;
    u64 seqId = 0;
    bool gpuOutstandingCounted = false;
    EncodeSessionSourceList fixedCompletionSources{};
    std::optional<encoders::EncodedCompletionSpan> completionSpanShadow{};
    std::span<const QueueCompletionSource> explicitCompletionSourceSpan()
        const noexcept {
      return fixedCompletionSources.span();
    }
    bool assignFixedCompletionSources(
        std::span<const QueueCompletionSource> sources) noexcept {
      QueueSubmissionRecord staged;
      if (!staged.assignFixedCompletionSources(sources)) {
        return false;
      }
      fixedCompletionSources = staged.fixedCompletionSources;
      completionSpanShadow = staged.completionSpanShadow;
      return true;
    }
    bool completionSpanShadowMatchesSources() const noexcept {
      return encodedCompletionSpanShadowMatchesProjection(
          completionSpanShadow, explicitCompletionSourceSpan());
    }
    u64 commandBufferChainLength = 1;
    std::chrono::steady_clock::time_point enqueueTime{};
    WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
    std::vector<QueueSubmissionRecord::RenderEncoderGpuSample> renderEncoderGpuSamples{};
    std::vector<std::function<void()>> completionCallbacks;
    std::vector<std::shared_ptr<void>> retainedPayloads;
  };

  // Drain one pending completion — blocks on waitUntilCompleted() and then
  // runs the diagnostics / completedSeqQueue / transition work. Called from
  // the dedicated completion-watcher thread. Returns true if a record was
  // processed, false after the controller-owned pending-stop latch is set and
  // the queue is empty.
  bool processOnePendingCompletion();
  ArenaGroupSettlementLedger* completedArenaGroupSettlementLedger() noexcept {
    return &completedArenaGroupSettlements_;
  }
  std::uint64_t completedEventSettlementCount() const noexcept {
    return completedEventSettlementCount_;
  }
  std::uint64_t completedEventTailSeqId() const noexcept {
    return completedEventTailSeqId_;
  }
  const std::optional<CpuReadyTape::ArenaGroupSettlement>&
  lastCompletedEventSettlement() const noexcept {
    return lastCompletedEventSettlement_;
  }
  // CPU-only specs use this to exercise the completion-watcher expansion
  // path without manufacturing a fake Objective-C command-buffer handle.
  // Production submissions enter the same pending queue through submit().
  void enqueuePendingCompletionForTest(PendingCompletion pending);
  // Native-only deterministic completion seam. It is consumed after a
  // pending item is dequeued but before any completion-side effect.
  void forceNextCompletionFailureForTest() noexcept {
    std::lock_guard lock(pendingCompletionMutex_);
    testOnlyForceNextCompletionFailure_ = true;
  }
  std::size_t pendingCompletionCountForTest() noexcept {
    std::lock_guard lock(pendingCompletionMutex_);
    return pendingCompletion_.size();
  }
  std::optional<PostEncodeCompletionReceipt> postEncodeReceiptForTest(
      u64 seqId, PostEncodeReceiptState state) const noexcept {
    return postEncodeCompletionLedger_.receiptFor(seqId, state);
  }

  // Publish the completion predicate while holding its owning mutex, then
  // notify. This may be called with the scheduling mutex held; the completion
  // thread never takes the locks in the opposite order.
  void requestPendingCompletionStop() noexcept;
  void resetPendingCompletionStop() noexcept;
  using PendingCompletionWaitObserverForTest = void (*)(void*) noexcept;
  void setPendingCompletionWaitObserverForTest(
      void* context, PendingCompletionWaitObserverForTest observer) noexcept {
    pendingCompletionWaitObserverContext_ = context;
    pendingCompletionWaitObserver_ = observer;
  }

 private:
  struct CpuReadySupplyObservation {
    CpuReadyTape::PayloadKind sourceClass =
        CpuReadyTape::PayloadKind::Legacy;
    CpuReadySourceId sourceId{};
    CpuReadyStorageRef storage{};
    std::uint32_t controlIndex = std::numeric_limits<std::uint32_t>::max();
    u64 seqId = 0;
    CpuReadySupplyObservationToken attemptToken{};
    std::chrono::steady_clock::time_point replayEntryTime{};
    std::chrono::steady_clock::time_point publishTime{};

    bool matchesSource(CpuReadyTape::PayloadKind kind, CpuReadySourceId id,
                       CpuReadyStorageRef storageRef,
                       std::uint32_t control) const noexcept {
      return sourceClass == kind && sourceId == id && storage == storageRef &&
             controlIndex == control;
    }

    bool matchesExact(CpuReadyTape::PayloadKind kind, CpuReadySourceId id,
                      CpuReadyStorageRef storageRef,
                      std::uint32_t control, u64 sequence) const noexcept {
      return matchesSource(kind, id, storageRef, control) &&
             seqId == sequence;
    }
  };

  static constexpr std::size_t kCpuReadySupplyObservationCapacity = 64;

  // These helpers are called while pendingCompletionMutex_ is held.  The
  // identity includes both Tape generations and the control-slot index so a
  // recycled source can never consume an old sample.  The array is fixed and
  // intentionally diagnostic-only; dropping an unmatched observation is
  // reported as coverage loss instead of affecting scheduling.
  CpuReadySupplyObservationToken recordCpuReadySupplyReplayEntryIdentityLocked(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId,
      CpuReadySupplyObservationToken attemptToken);
  void recordCpuReadySupplyPublishedLocked(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId);
  void recordCpuReadySupplyDequeuedLocked(
      CpuReadyTape::PayloadKind sourceClass, CpuReadySourceId sourceId,
      CpuReadyStorageRef storage, std::uint32_t controlIndex, u64 seqId);

  void resetNoEnqueueGapProgressLocked();

  std::mutex pendingCompletionMutex_{};
  std::condition_variable pendingCompletionCv_{};
  std::deque<PendingCompletion> pendingCompletion_{};
  bool pendingCompletionStop_ = false;
  bool testOnlyForceNextCompletionFailure_ = false;
  void* pendingCompletionWaitObserverContext_ = nullptr;
  PendingCompletionWaitObserverForTest pendingCompletionWaitObserver_ =
      nullptr;
  bool completionWaitActive_ = false;
  std::uint32_t producerSequenceWaitDepth_ = 0;
  u64 producerSequenceWaitTargetSeqId_ = 0;
  std::uint32_t producerWriterPressureDepth_ = 0;
  std::uint64_t completionWaitEnqueues_ = 0;
  std::chrono::steady_clock::time_point completionWaitCommitPublishTime_{};
  std::chrono::steady_clock::time_point completionWaitEncodeDequeueTime_{};
  std::chrono::steady_clock::time_point lastNoEnqueueCompletionWaitEnd_{};
  bool noEnqueueGapCommitPublishRecorded_ = false;
  bool noEnqueueGapEncodeDequeueRecorded_ = false;
  bool noEnqueueGapCommandBufferCommitRecorded_ = false;
  bool noEnqueueGapCommitChunkEntryRecorded_ = false;
  bool noEnqueueGapCommitChunkReplayStartRecorded_ = false;
  bool noEnqueueGapCommitChunkReplayEndRecorded_ = false;
  bool noEnqueueGapCommitPublishOnBeforePublishRecorded_ = false;
  std::chrono::steady_clock::time_point noEnqueueGapCommitChunkEntryTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapCommitPublishTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapEncodeDequeueTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapLastCommitChunkReplayEndTime_{};
  std::uint64_t noEnqueueGapCommitChunkEntriesBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkReplayStartsBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkReplayEndsBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkCompletedReplayCpuBeforePublishNs_ = 0;
  std::uint64_t noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_ = 0;
  std::uint64_t noEnqueueGapCommitChunkInterReplayGapBeforePublishNs_ = 0;
  std::unique_ptr<CpuReadySupplyObservation[]>
      cpuReadySupplyObservations_{};
  u64 nextCpuReadySupplyObservationToken_ = 1u;
};

class CompletionTracker {
 public:
  bool inspect(obj_handle_t commandBuffer, const CommandBufferDiagnostics& diagnostics, const char* context);
  const std::string& lastErrorSummary() const noexcept { return lastErrorSummary_; }

 private:
  std::string commandBufferStatusName(WMTCommandBufferStatus status) const;

  std::string lastErrorSummary_;
};

}  // namespace dxmt9::core::metalqueue
