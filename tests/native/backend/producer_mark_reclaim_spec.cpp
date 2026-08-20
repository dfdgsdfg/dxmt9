// Producer mark / reclaim shared-predicate + trace-conformance spec.
//
// Binds `specs/verification/tla/ProducerMarkReclaim.tla` to C++ through the
// two shared pure predicates in `src/dxmt9/dxmt9_mark_reclaim_predicates.hpp`.
// See `docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`
// §5 (semantic-isomorphism harness) and §8 (the T2a/T2b relaxations the model
// licenses).
//
// Two sections:
//
//   1. Truth tables over the boundary cases of `canReclaimRecord` and
//      `markStampUpper`. These pin the predicates themselves.
//
//   2. Trace conformance. A miniature state machine mirrors the model's
//      variables and takes the model's actions as steps; every transition
//      that has a predicate in the model calls the SAME C++ predicate the
//      production code calls. Representative interleavings — including the
//      TLC counterexample's shape — are hand-encoded as step sequences and
//      replayed.
//
// ---------------------------------------------------------------------------
// TRANSLATION CONVENTION (TLA action -> native Step)
// ---------------------------------------------------------------------------
// A TLC trace prints one action name per state transition, e.g.
//
//     State 2: <PinChunkResources line 157, ... of module ProducerMarkReclaim>
//     State 3: <SetDestroyPending line 322, ... of module ProducerMarkReclaim>
//     State 4: <Reclaim line 342, ... of module ProducerMarkReclaim>
//
// Each such line maps to exactly one `Step` below:
//
//   TLA action              native Step
//   ----------------------  --------------------------------------------------
//   PinChunkResources(S)    {Action::PinChunkResources, resourceMask(S)}
//   BeginMark               {Action::BeginMark}
//   StampMark(r)            {Action::StampMark, r}
//   CaptureRead(r)          {Action::CaptureRead, r}
//   EndCommit               {Action::EndCommit}
//   ReleasePins             {Action::ReleasePins}
//   MapFastRead             {Action::MapFastRead, observedValue}
//   SlotAdvance             {Action::SlotAdvance}
//   WorkerBeginBatch(S)     {Action::WorkerBeginBatch, resourceMask(S)}
//   WorkerStampMark(r)      {Action::WorkerStampMark, r}
//   WorkerEndStamping       {Action::WorkerEndStamping}
//   WorkerRestamp           {Action::WorkerRestamp}
//   WorkerAppend            {Action::WorkerAppend}
//   WorkerReleaseBatchRefs  {Action::WorkerReleaseBatchRefs}
//   WorkerRetireBatch       {Action::WorkerRetireBatch}
//   WorkerReleaseRefs       {Action::WorkerReleaseRefs}
//   SetDestroyPending(r)    {Action::SetDestroyPending, r}
//   Reclaim(r)              {Action::Reclaim, r}
//   AdvanceCompleted        {Action::AdvanceCompleted}
//
// The single-resource argument `r` becomes the `resource` field; a set-valued
// argument becomes a bitmask over resource indices. Only `MapFastRead` needs
// the value TLC chose, because that action is nondeterministic in the model.
//
// Rules that make the replay meaningful:
//
//   * `Machine::step` returns false when the model's action guard is not
//     enabled. A trace whose guards do not all hold is a translation error,
//     not a finding — `replay()` fails the test on it.
//   * `Machine::useAfterFree` mirrors the model's sticky fault flag and is set
//     at exactly the sites the model sets it (StampMark / CaptureRead /
//     WorkerStampMark / WorkerRestamp on a freed record, Reclaim of a record
//     a live commit window or a live worker batch still needs).
//   * `PinDiscipline` and `RestampDiscipline` are constructor arguments,
//     matching the model constants. `Enforced` is production; `Removed` is the
//     corresponding counterexample world. They are independent: each has its
//     own `.counterexample.cfg` and its own translated trace below.
//
// A green production model plus a red counterexample model plus these replays
// mean the model and the code agree on the transition vocabulary. It does NOT
// mean the C++ atomics are correctly ordered — that obligation belongs to the
// deterministic interleaving harness (design §5 layer 3, R-VERIF-7.3) and is
// still open.

#include "../../../src/dxmt9/dxmt9_mark_reclaim_predicates.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using dxmt9::resources::canReclaimRecord;
using dxmt9::resources::markStampUpper;

using u64 = std::uint64_t;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (left != right)";
    fail(out.str());
  }
}

// ===========================================================================
// 1. Predicate truth tables
// ===========================================================================

// TLA+: ProducerMarkReclaim!CanReclaimRecord — the enabling condition of
// Reclaim(r). Both conjuncts are load-bearing and the watermark comparison is
// inclusive.
void testCanReclaimRecordTruthTable() {
  // destroyPending = false: never reclaimable, whatever the watermark says.
  check(!canReclaimRecord(false, 0, 0), "live record at equal watermark");
  check(!canReclaimRecord(false, 0, 7), "live record far behind watermark");
  check(!canReclaimRecord(false, 7, 0), "live record ahead of watermark");

  // destroyPending = true: the watermark decides, inclusively.
  check(canReclaimRecord(true, 0, 0), "pending record, both at origin");
  check(canReclaimRecord(true, 3, 3), "pending record exactly at watermark");
  check(canReclaimRecord(true, 3, 4), "pending record behind watermark");
  check(!canReclaimRecord(true, 4, 3), "pending record one ahead of watermark");
  check(!canReclaimRecord(true, 1, 0), "pending record still in flight");

  // The boundary is `<=`, not `<`: a record last used by chunk N becomes
  // reclaimable the moment completedSeqId reaches N, not N+1.
  check(!canReclaimRecord(true, 5, 4), "watermark one short of last use");
  check(canReclaimRecord(true, 5, 5), "watermark exactly reaches last use");

  // Saturation ends: an unmarked record (lastUsedSeqId == 0) is reclaimable as
  // soon as it is pending, and a record marked at the domain maximum is not
  // reclaimable until the watermark gets there.
  check(canReclaimRecord(true, 0, 0), "never-marked pending record");
  const u64 maxSeq = ~u64{0};
  check(!canReclaimRecord(true, maxSeq, maxSeq - 1), "max last use, watermark short");
  check(canReclaimRecord(true, maxSeq, maxSeq), "max last use, watermark equal");
}

// TLA+: ProducerMarkReclaim!MarkStampUpper — the value StampMark(r) writes.
// A monotone max: order-independent, idempotent, never regressing.
void testMarkStampUpperTruthTable() {
  checkEq(markStampUpper(0, 0), u64{0}, "origin stamp");
  checkEq(markStampUpper(0, 1), u64{1}, "first stamp raises");
  checkEq(markStampUpper(3, 5), u64{5}, "later stamp raises");
  checkEq(markStampUpper(5, 3), u64{5}, "earlier stamp does not regress");
  checkEq(markStampUpper(4, 4), u64{4}, "equal stamp is a no-op");

  // Idempotence: re-stamping with the value already stored changes nothing.
  const u64 once = markStampUpper(2, 9);
  checkEq(markStampUpper(once, 9), once, "re-stamp is idempotent");

  // Order independence: two concurrent marks landing in either order agree.
  checkEq(markStampUpper(markStampUpper(0, 7), 4),
          markStampUpper(markStampUpper(0, 4), 7),
          "concurrent stamps commute");

  const u64 maxSeq = ~u64{0};
  checkEq(markStampUpper(maxSeq, 1), maxSeq, "max is absorbing");
  checkEq(markStampUpper(1, maxSeq), maxSeq, "max wins from either side");
}

// ===========================================================================
// 2. Trace conformance
// ===========================================================================

constexpr std::size_t kResources = 2;  // matches CONSTANT Resources = {r1, r2}
constexpr u64 kMaxSeqId = 3;           // matches CONSTANT MAX_SEQID = 3

enum class PinDiscipline { Enforced, Removed };
enum class RestampDiscipline { Enforced, Removed };

// Mirrors ProducerMarkReclaim!CommitPhases.
enum class CommitPhase { Idle, Pinned, Marking, Committed };

// Mirrors ProducerMarkReclaim!WorkerPhases. `Marked` is the only phase in
// which the worker owns CommandQueue::mutex_, which is why SlotAdvance is
// disabled there and the re-stamp is a fixed point.
enum class WorkerPhase { Idle, Marking, Marked, Appended, Pending };

enum class Action {
  PinChunkResources,
  BeginMark,
  StampMark,
  CaptureRead,
  EndCommit,
  ReleasePins,
  MapFastRead,
  SlotAdvance,
  WorkerBeginBatch,
  WorkerStampMark,
  WorkerEndStamping,
  WorkerRestamp,
  WorkerAppend,
  WorkerReleaseBatchRefs,
  WorkerRetireBatch,
  WorkerReleaseRefs,
  SetDestroyPending,
  Reclaim,
  AdvanceCompleted,
};

struct Step {
  Action action{};
  // `resource` carries a single resource index for the per-resource actions,
  // a resource bitmask for PinChunkResources, and the chosen value for
  // MapFastRead. Unused otherwise.
  std::uint32_t argument = 0;
};

constexpr std::uint32_t mask(std::size_t index) {
  return std::uint32_t{1} << index;
}

const char* actionName(Action action) {
  switch (action) {
    case Action::PinChunkResources: return "PinChunkResources";
    case Action::BeginMark:         return "BeginMark";
    case Action::StampMark:         return "StampMark";
    case Action::CaptureRead:       return "CaptureRead";
    case Action::EndCommit:         return "EndCommit";
    case Action::ReleasePins:       return "ReleasePins";
    case Action::MapFastRead:       return "MapFastRead";
    case Action::SlotAdvance:       return "SlotAdvance";
    case Action::WorkerBeginBatch:  return "WorkerBeginBatch";
    case Action::WorkerStampMark:   return "WorkerStampMark";
    case Action::WorkerEndStamping: return "WorkerEndStamping";
    case Action::WorkerRestamp:     return "WorkerRestamp";
    case Action::WorkerAppend:      return "WorkerAppend";
    case Action::WorkerReleaseBatchRefs: return "WorkerReleaseBatchRefs";
    case Action::WorkerRetireBatch: return "WorkerRetireBatch";
    case Action::WorkerReleaseRefs: return "WorkerReleaseRefs";
    case Action::SetDestroyPending: return "SetDestroyPending";
    case Action::Reclaim:           return "Reclaim";
    case Action::AdvanceCompleted:  return "AdvanceCompleted";
  }
  return "<unknown>";
}

// One-to-one with the model's VARIABLES. Transitions that the model expresses
// through CanReclaimRecord / MarkStampUpper call the shared C++ predicates.
class Machine {
 public:
  explicit Machine(PinDiscipline discipline,
                   RestampDiscipline restamp = RestampDiscipline::Enforced)
      : discipline_(discipline), restamp_(restamp) {}

  bool useAfterFree() const noexcept { return useAfterFree_; }
  u64 lastUsedSeqId(std::size_t r) const noexcept { return lastUsedSeqId_[r]; }
  u64 completedSeqId() const noexcept { return completedSeqId_; }
  u64 observedCompletedSeqId() const noexcept { return observedCompletedSeqId_; }
  bool freed(std::size_t r) const noexcept { return freed_[r]; }
  bool destroyPending(std::size_t r) const noexcept { return destroyPending_[r]; }
  bool pinned(std::size_t r) const noexcept {
    return retainerPinned_[r] || workerPinned_[r];
  }
  CommitPhase commitPhase() const noexcept { return phase_; }
  WorkerPhase workerPhase() const noexcept { return workerPhase_; }
  u64 workerTicket() const noexcept { return workerTicket_; }
  u64 workerAppendSeqId() const noexcept { return workerAppendSeqId_; }
  u64 nextSeqId() const noexcept { return nextSeqId_; }

  // TLA+: ProducerMarkReclaim!WorkerAppendCoveredByStamps — the re-stamp
  // obligation, stated directly rather than through the fault flag.
  bool workerAppendCoveredByStamps() const noexcept {
    if (workerPhase_ != WorkerPhase::Appended &&
        workerPhase_ != WorkerPhase::Pending) {
      return true;
    }
    for (std::size_t r = 0; r < kResources; ++r) {
      if ((workerBatch_ & mask(r)) == 0) continue;
      if (lastUsedSeqId_[r] < workerAppendSeqId_) return false;
    }
    return true;
  }

  // TLA+: ProducerMarkReclaim!NoCaptureAfterFree — T2b's existence
  // obligation on its own, stated over `captured` rather than through the
  // shared fault flag so a capture-side violation is separately observable.
  bool noCaptureAfterFree() const noexcept {
    for (std::size_t r = 0; r < kResources; ++r) {
      if ((captured_ & mask(r)) != 0 && freed_[r]) return false;
    }
    return true;
  }

  // TLA+: ProducerMarkReclaim!StampsPrecedeCapture — spec §4's third ordering
  // protocol, which T2b turns from a side effect of one mutex hold into a real
  // obligation.
  bool stampsPrecedeCapture() const noexcept {
    return (captured_ & ~marked_) == 0;
  }

  // Returns false when the model's guard for `step` is not enabled — i.e. the
  // encoded trace is not a behaviour of the spec.
  bool step(const Step& s);

 private:
  bool commitInFlight() const noexcept { return phase_ != CommitPhase::Idle; }
  u64 chunkSeqId() const noexcept { return commitSeqId_; }
  bool inChunk(std::size_t r) const noexcept {
    return (chunkNamed_ & mask(r)) != 0;
  }
  bool workerHoldsQueueMutex() const noexcept {
    return workerPhase_ == WorkerPhase::Marked;
  }
  // TLA+: ProducerMarkReclaim!WorkerRecordInUse(r).
  bool workerRecordInUse(std::size_t r) const noexcept {
    if ((workerBatch_ & mask(r)) == 0) return false;
    switch (workerPhase_) {
      case WorkerPhase::Marking:
      case WorkerPhase::Marked:
      case WorkerPhase::Appended:
        return true;
      case WorkerPhase::Pending:
        return workerAppendSeqId_ > completedSeqId_;
      case WorkerPhase::Idle:
        return false;
    }
    return false;
  }
  void stampWorkerBatch(u64 seqId) {
    for (std::size_t r = 0; r < kResources; ++r) {
      if ((workerBatch_ & mask(r)) == 0) continue;
      lastUsedSeqId_[r] = markStampUpper(lastUsedSeqId_[r], seqId);
      useAfterFree_ = useAfterFree_ || freed_[r];
    }
  }

  PinDiscipline discipline_;
  RestampDiscipline restamp_;
  std::array<bool, kResources> retainerPinned_{};
  std::array<bool, kResources> workerPinned_{};
  std::array<bool, kResources> destroyPending_{};
  std::array<bool, kResources> freed_{};
  std::array<u64, kResources> lastUsedSeqId_{};
  u64 nextSeqId_ = 1;
  u64 completedSeqId_ = 0;
  u64 observedCompletedSeqId_ = 0;
  CommitPhase phase_ = CommitPhase::Idle;
  u64 commitSeqId_ = 0;
  std::uint32_t chunkNamed_ = 0;
  std::uint32_t marked_ = 0;
  std::uint32_t captured_ = 0;
  WorkerPhase workerPhase_ = WorkerPhase::Idle;
  std::uint32_t workerBatch_ = 0;
  std::uint32_t workerStamped_ = 0;
  u64 workerTicket_ = 0;
  u64 workerAppendSeqId_ = 0;
  bool useAfterFree_ = false;
};

bool Machine::step(const Step& s) {
  switch (s.action) {
    case Action::PinChunkResources: {
      // TLA+: PinChunkResources(S)
      if (phase_ != CommitPhase::Idle) return false;
      const std::uint32_t named = s.argument;
      if (named == 0) return false;
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((named & mask(r)) == 0) continue;
        if (destroyPending_[r] || freed_[r]) return false;
      }
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((named & mask(r)) != 0) retainerPinned_[r] = true;
      }
      chunkNamed_ = named;
      phase_ = CommitPhase::Pinned;
      return true;
    }
    case Action::BeginMark: {
      // TLA+: BeginMark — the commit reserves its ticket AND owns the chunk
      // published under it, so `commitSeqId_` cannot move inside the window.
      if (phase_ != CommitPhase::Pinned) return false;
      if (nextSeqId_ > kMaxSeqId) return false;
      commitSeqId_ = nextSeqId_;
      nextSeqId_ += 1;
      phase_ = CommitPhase::Marking;
      return true;
    }
    case Action::StampMark: {
      // TLA+: StampMark(r) — the shared predicate is the transition.
      const std::size_t r = s.argument;
      if (r >= kResources) return false;
      if (phase_ != CommitPhase::Marking) return false;
      if (!inChunk(r)) return false;
      if ((marked_ & mask(r)) != 0) return false;
      marked_ |= mask(r);
      lastUsedSeqId_[r] = markStampUpper(lastUsedSeqId_[r], chunkSeqId());
      useAfterFree_ = useAfterFree_ || freed_[r];
      return true;
    }
    case Action::CaptureRead: {
      // TLA+: CaptureRead(r)
      const std::size_t r = s.argument;
      if (r >= kResources) return false;
      if (phase_ != CommitPhase::Marking) return false;
      if ((marked_ & mask(r)) == 0) return false;
      if ((captured_ & mask(r)) != 0) return false;
      captured_ |= mask(r);
      useAfterFree_ = useAfterFree_ || freed_[r];
      return true;
    }
    case Action::EndCommit: {
      // TLA+: EndCommit
      if (phase_ != CommitPhase::Marking) return false;
      if (marked_ != chunkNamed_) return false;
      if (captured_ != chunkNamed_) return false;
      phase_ = CommitPhase::Committed;
      return true;
    }
    case Action::ReleasePins: {
      // TLA+: ReleasePins — retainer hands the refs to the replay worker.
      if (phase_ != CommitPhase::Committed) return false;
      for (std::size_t r = 0; r < kResources; ++r) {
        if (!inChunk(r)) continue;
        retainerPinned_[r] = false;
        workerPinned_[r] = true;
      }
      phase_ = CommitPhase::Idle;
      chunkNamed_ = 0;
      marked_ = 0;
      captured_ = 0;
      return true;
    }
    case Action::MapFastRead: {
      // TLA+: MapFastRead — the chosen value must be in
      // (observedCompletedSeqId, completedSeqId].
      const u64 value = s.argument;
      if (value <= observedCompletedSeqId_) return false;
      if (value > completedSeqId_) return false;
      observedCompletedSeqId_ = value;
      return true;
    }
    case Action::SlotAdvance: {
      // TLA+: SlotAdvance — another actor force-publishes the open writing
      // slot, raising the seq a pending append will get. Excluded against the
      // one phase in which the worker owns the queue mutex, because every
      // writer of `nextSeqId_` needs that mutex.
      if (workerHoldsQueueMutex()) return false;
      if (nextSeqId_ > kMaxSeqId) return false;
      nextSeqId_ += 1;
      return true;
    }
    case Action::WorkerBeginBatch: {
      // TLA+: WorkerBeginBatch(S) — the ticket is an acquire load taken
      // WITHOUT the queue mutex.
      if (workerPhase_ != WorkerPhase::Idle) return false;
      const std::uint32_t named = s.argument;
      if (named == 0) return false;
      if (nextSeqId_ > kMaxSeqId) return false;
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((named & mask(r)) == 0) continue;
        if (destroyPending_[r] || freed_[r]) return false;
      }
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((named & mask(r)) != 0) workerPinned_[r] = true;
      }
      workerBatch_ = named;
      workerStamped_ = 0;
      workerTicket_ = nextSeqId_;
      workerPhase_ = WorkerPhase::Marking;
      return true;
    }
    case Action::WorkerStampMark: {
      // TLA+: WorkerStampMark(r) — the shared predicate is the transition,
      // exactly as in the producer's StampMark.
      const std::size_t r = s.argument;
      if (r >= kResources) return false;
      if (workerPhase_ != WorkerPhase::Marking) return false;
      if ((workerBatch_ & mask(r)) == 0) return false;
      if ((workerStamped_ & mask(r)) != 0) return false;
      workerStamped_ |= mask(r);
      lastUsedSeqId_[r] = markStampUpper(lastUsedSeqId_[r], workerTicket_);
      useAfterFree_ = useAfterFree_ || freed_[r];
      return true;
    }
    case Action::WorkerEndStamping: {
      // TLA+: WorkerEndStamping — the unlocked window closes.
      if (workerPhase_ != WorkerPhase::Marking) return false;
      if (workerStamped_ != workerBatch_) return false;
      workerPhase_ = WorkerPhase::Marked;
      return true;
    }
    case Action::WorkerRestamp: {
      // TLA+: WorkerRestamp — THE PROTOCOL. Deleted entirely under
      // RestampDiscipline::Removed.
      if (restamp_ != RestampDiscipline::Enforced) return false;
      if (workerPhase_ != WorkerPhase::Marked) return false;
      if (workerTicket_ == nextSeqId_) return false;
      stampWorkerBatch(nextSeqId_);
      workerTicket_ = nextSeqId_;
      return true;
    }
    case Action::WorkerAppend: {
      // TLA+: WorkerAppend — the records enter the open slot, which will be
      // published as `nextSeqId_`.
      if (workerPhase_ != WorkerPhase::Marked) return false;
      if (restamp_ == RestampDiscipline::Enforced &&
          workerTicket_ != nextSeqId_) {
        return false;
      }
      workerAppendSeqId_ = nextSeqId_;
      workerPhase_ = WorkerPhase::Appended;
      return true;
    }
    case Action::WorkerReleaseBatchRefs: {
      // TLA+: WorkerReleaseBatchRefs — `releaseRetainedWrappers` after replay,
      // which is BEFORE the appended chunk completes. From here the seq stamp,
      // not the pin, is the only thing protecting the record.
      if (workerPhase_ != WorkerPhase::Appended) return false;
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((workerBatch_ & mask(r)) != 0) workerPinned_[r] = false;
      }
      workerPhase_ = WorkerPhase::Pending;
      return true;
    }
    case Action::WorkerRetireBatch: {
      // TLA+: WorkerRetireBatch
      if (workerPhase_ != WorkerPhase::Pending) return false;
      if (completedSeqId_ < workerAppendSeqId_) return false;
      workerPhase_ = WorkerPhase::Idle;
      workerBatch_ = 0;
      workerStamped_ = 0;
      workerTicket_ = 0;
      workerAppendSeqId_ = 0;
      return true;
    }
    case Action::WorkerReleaseRefs: {
      // TLA+: WorkerReleaseRefs — refs inherited from an EARLIER chunk. Never
      // touches the batch the worker is currently replaying.
      bool any = false;
      for (std::size_t r = 0; r < kResources; ++r) {
        any = any || (workerPinned_[r] && (workerBatch_ & mask(r)) == 0);
      }
      if (!any) return false;
      for (std::size_t r = 0; r < kResources; ++r) {
        if ((workerBatch_ & mask(r)) == 0) workerPinned_[r] = false;
      }
      return true;
    }
    case Action::SetDestroyPending: {
      // TLA+: SetDestroyPending(r) — `!pinned` IS the pin-ordering premise,
      // and the ONLY thing PinDiscipline::Removed deletes.
      const std::size_t r = s.argument;
      if (r >= kResources) return false;
      if (destroyPending_[r] || freed_[r]) return false;
      if (discipline_ == PinDiscipline::Enforced && pinned(r)) return false;
      destroyPending_[r] = true;
      return true;
    }
    case Action::Reclaim: {
      // TLA+: Reclaim(r) — the shared predicate is the guard.
      const std::size_t r = s.argument;
      if (r >= kResources) return false;
      if (freed_[r]) return false;
      if (!canReclaimRecord(destroyPending_[r], lastUsedSeqId_[r],
                            completedSeqId_)) {
        return false;
      }
      freed_[r] = true;
      useAfterFree_ = useAfterFree_ ||
                      (commitInFlight() && inChunk(r)) ||
                      workerRecordInUse(r);
      return true;
    }
    case Action::AdvanceCompleted: {
      // TLA+: AdvanceCompleted
      if (completedSeqId_ + 1 >= nextSeqId_) return false;
      completedSeqId_ += 1;
      return true;
    }
  }
  return false;
}

// Replays a translated TLC trace. A step whose guard is not enabled means the
// translation is wrong, so it fails the test rather than being ignored.
template <std::size_t N>
Machine replay(PinDiscipline discipline,
               const std::array<Step, N>& steps,
               std::string_view traceName,
               RestampDiscipline restamp = RestampDiscipline::Enforced) {
  Machine machine(discipline, restamp);
  for (std::size_t i = 0; i < N; ++i) {
    if (!machine.step(steps[i])) {
      std::ostringstream out;
      out << traceName << ": step " << i << " (" << actionName(steps[i].action)
          << ", arg=" << steps[i].argument << ") was not enabled";
      fail(out.str());
    }
  }
  return machine;
}

constexpr std::size_t kR1 = 0;
constexpr std::size_t kR2 = 1;

// Trace A — the production happy path. One chunk names both resources, marks
// and captures each, commits, and hands the pins to the replay worker. Nothing
// is ever destroyed, so nothing can be reclaimed.
void testTraceHappyPathCommit() {
  const std::array<Step, 9> steps{{
      {Action::PinChunkResources, mask(kR1) | mask(kR2)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
      {Action::StampMark, kR2},
      {Action::CaptureRead, kR2},
      {Action::EndCommit},
      {Action::ReleasePins},
      {Action::WorkerReleaseRefs},
  }};
  const Machine machine = replay(PinDiscipline::Enforced, steps, "happy-path");

  check(!machine.useAfterFree(), "happy path must not fault");
  checkEq(machine.lastUsedSeqId(kR1), u64{1}, "r1 stamped with chunk ticket 1");
  checkEq(machine.lastUsedSeqId(kR2), u64{1}, "r2 stamped with chunk ticket 1");
  check(!machine.pinned(kR1), "worker released r1");
  check(!machine.pinned(kR2), "worker released r2");
  checkEq(static_cast<int>(machine.commitPhase()),
          static_cast<int>(CommitPhase::Idle), "commit window closed");
}

// Trace B — T2a/T2b concurrency, the case the relaxation exists for. While the
// producer is mid-mark on chunk 2, the replay-offload worker drops its refs on
// chunk 1's resources and the completion thread advances the watermark. r2 is
// not named by chunk 2, so it legitimately reaches destroyPending and is
// reclaimed *while the producer is marking r1*. No fault: reclaim of a record
// outside the live chunk is exactly what T2a licenses.
void testTraceWorkerReclaimsDuringMarkWindow() {
  const std::array<Step, 15> steps{{
      // chunk 1 names both resources.
      {Action::PinChunkResources, mask(kR1) | mask(kR2)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
      {Action::StampMark, kR2},
      {Action::CaptureRead, kR2},
      {Action::EndCommit},
      {Action::ReleasePins},
      // chunk 2 names only r1.
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      // ... and now the other two actors run inside chunk 2's mark window.
      {Action::WorkerReleaseRefs},
      {Action::AdvanceCompleted},
      {Action::SetDestroyPending, kR2},
      {Action::Reclaim, kR2},
      {Action::StampMark, kR1},
  }};
  const Machine machine =
      replay(PinDiscipline::Enforced, steps, "worker-reclaim-during-mark");

  check(!machine.useAfterFree(), "reclaim outside the chunk must not fault");
  check(machine.freed(kR2), "r2 was reclaimed");
  check(!machine.freed(kR1), "r1 stayed live");
  checkEq(machine.lastUsedSeqId(kR1), u64{2}, "r1 restamped with ticket 2");
  checkEq(machine.completedSeqId(), u64{1}, "chunk 1 completed");
}

// Trace C — the pin premise doing its job. Same interleaving as trace B, but
// the resource the worker releases is ALSO named by the live chunk. The
// retainer pin still covers it, so SetDestroyPending is not enabled and the
// reclaim can never start.
void testTraceRetainerPinBlocksDestroyPending() {
  const std::array<Step, 12> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
      {Action::EndCommit},
      {Action::ReleasePins},
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      // The worker drops chunk 1's ref on r1 mid-window; the retainer's own
      // ref for chunk 2 is still held.
      {Action::WorkerReleaseRefs},
      {Action::AdvanceCompleted},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
  }};
  Machine machine =
      replay(PinDiscipline::Enforced, steps, "retainer-pin-blocks-destroy");

  check(machine.pinned(kR1), "retainer still pins r1");
  // The premise, stated as an enabledness fact: destroy-pending cannot be set.
  check(!machine.step({Action::SetDestroyPending, kR1}),
        "SetDestroyPending must be disabled while r1 is retainer-pinned");
  check(!machine.destroyPending(kR1), "r1 never became destroy-pending");
  // And with destroyPending false the reclaim gate is closed regardless of the
  // watermark — this is the shared predicate, not a restatement of it.
  check(!canReclaimRecord(machine.destroyPending(kR1),
                          machine.lastUsedSeqId(kR1), machine.completedSeqId()),
        "reclaim gate closed for a pinned record");
  check(!machine.step({Action::Reclaim, kR1}), "Reclaim must be disabled");
  check(!machine.useAfterFree(), "production discipline must not fault");
}

// Trace D — the TLC counterexample, translated verbatim. With the pin premise
// removed, destroy-pending lands on a retainer-pinned, chunk-named record; the
// watermark is already past its (still zero) last use; the reclaim frees a
// record the producer is about to mark. `useAfterFree` trips at the Reclaim
// step, which is TLC's minimal 4-state trace.
//
//   State 2: PinChunkResources({r1})
//   State 3: SetDestroyPending(r1)
//   State 4: Reclaim(r1)            <- Invariant NoUseAfterFree is violated
void testTraceCounterexampleUseAfterFree() {
  const std::array<Step, 3> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::SetDestroyPending, kR1},
      {Action::Reclaim, kR1},
  }};
  const Machine machine =
      replay(PinDiscipline::Removed, steps, "tlc-counterexample");

  check(machine.freed(kR1), "r1 was reclaimed inside the commit window");
  check(machine.useAfterFree(),
        "counterexample trace must reproduce the use-after-free fault");
}

// Trace E — the counterexample continued past TLC's stopping point, to show
// the second fault site: the producer's own StampMark and CaptureRead land on
// a record that no longer exists. This is the shape the design brief names
// ("a late stamp"); it is unreachable under the production discipline, which
// trace C proves by enabledness.
void testTraceCounterexampleLateStampAndCapture() {
  const std::array<Step, 6> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      {Action::SetDestroyPending, kR1},
      {Action::Reclaim, kR1},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
  }};
  const Machine machine =
      replay(PinDiscipline::Removed, steps, "tlc-counterexample-late-stamp");

  check(machine.useAfterFree(), "late stamp/capture on a freed record faults");
  // The stamp still ran and moved the watermark past the completed waterline —
  // the state `gcArena`'s DXMT_ASSERT is written to catch.
  checkEq(machine.lastUsedSeqId(kR1), u64{1}, "late stamp wrote the ticket");
  check(machine.lastUsedSeqId(kR1) > machine.completedSeqId(),
        "freed record's watermark now runs ahead of the GPU");

  // The same prefix under the production discipline cannot even start: the
  // pin premise disables step 2.
  Machine production(PinDiscipline::Enforced);
  check(production.step({Action::PinChunkResources, mask(kR1)}), "pin enabled");
  check(production.step({Action::BeginMark}), "begin mark enabled");
  check(!production.step({Action::SetDestroyPending, kR1}),
        "production discipline disables the counterexample's first bad step");
}

// Trace F — MapFastRead (T2c). The producer's atomic watermark read may be
// stale but is never ahead of the truth and never regresses.
void testTraceMapFastReadStaleButSound() {
  const std::array<Step, 11> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
      {Action::EndCommit},
      {Action::ReleasePins},
      {Action::PinChunkResources, mask(kR2)},
      {Action::BeginMark},
      {Action::AdvanceCompleted},
      {Action::MapFastRead, 1},
      {Action::WorkerReleaseRefs},
  }};
  Machine machine = replay(PinDiscipline::Enforced, steps, "map-fast-read");

  checkEq(machine.observedCompletedSeqId(), u64{1}, "producer observed 1");
  check(machine.observedCompletedSeqId() <= machine.completedSeqId(),
        "observed read never exceeds the truth");
  // Re-reading the same value is not a transition (the model requires strict
  // progress), and reading ahead of the truth is never enabled.
  check(!machine.step({Action::MapFastRead, 1}), "re-read of same value is not a step");
  check(!machine.step({Action::MapFastRead, 2}),
        "read ahead of completedSeqId must be disabled");
  check(!machine.useAfterFree(), "map fast path must not fault");
}

// Trace G — T2a', the re-stamp protocol doing its job. The replay worker reads
// its ticket (1) with the queue mutex released, and while it is stamping, some
// other actor force-publishes the open writing slot, so the seq its records
// will land under becomes 2. Under the production discipline the worker
// re-reads the frozen ticket after re-acquiring the mutex, re-stamps, and only
// then appends — so the stamps cover the chunk's final seq and the record
// survives long enough for the GPU to reach it.
void testTraceWorkerRestampCoversAdvancedSlotSeq() {
  const std::array<Step, 6> steps{{
      {Action::WorkerBeginBatch, mask(kR2)},  // ticket = 1, unlocked
      {Action::SlotAdvance},                  // force-publish: nextSeqId -> 2
      {Action::WorkerStampMark, kR2},         // stamps the STALE ticket 1
      {Action::WorkerEndStamping},            // re-acquires the queue mutex
      {Action::WorkerRestamp},                // frozen re-read: 1 != 2, restamp
      {Action::WorkerAppend},                 // records land under seq 2
  }};
  Machine machine =
      replay(PinDiscipline::Enforced, steps, "worker-restamp-covers-advance");

  checkEq(machine.workerAppendSeqId(), u64{2}, "records appended under seq 2");
  checkEq(machine.lastUsedSeqId(kR2), u64{2},
          "re-stamp raised the watermark to the chunk's final seq");
  check(machine.workerAppendCoveredByStamps(),
        "stamps must cover the seq the slot is published under");

  // A SlotAdvance is not enabled while the worker owns the mutex, which is
  // what makes the re-stamp a fixed point rather than another race. Replay the
  // prefix and check enabledness at the `Marked` phase directly.
  Machine frozen(PinDiscipline::Enforced);
  check(frozen.step({Action::WorkerBeginBatch, mask(kR2)}), "begin batch");
  check(frozen.step({Action::WorkerStampMark, kR2}), "stamp");
  check(frozen.step({Action::WorkerEndStamping}), "end stamping");
  check(!frozen.step({Action::SlotAdvance}),
        "SlotAdvance must be disabled while the worker holds the queue mutex");
  check(frozen.step({Action::WorkerAppend}), "append with an unmoved ticket");
  checkEq(frozen.lastUsedSeqId(kR2), frozen.workerAppendSeqId(),
          "unmoved ticket needs no re-stamp");

  // Continue trace G to completion: publish, complete, and only then is the
  // record reclaimable — the reclaim that faults in trace H is not enabled
  // here at the same watermark.
  check(machine.step({Action::WorkerReleaseBatchRefs}), "worker drops refs");
  check(machine.step({Action::SlotAdvance}), "the appended chunk is published");
  check(machine.step({Action::AdvanceCompleted}), "GPU completes seq 1");
  check(machine.step({Action::SetDestroyPending, kR2}), "last ref dropped");
  check(!canReclaimRecord(machine.destroyPending(kR2),
                          machine.lastUsedSeqId(kR2), machine.completedSeqId()),
        "re-stamped record is NOT reclaimable while its chunk is pending");
  check(!machine.step({Action::Reclaim, kR2}), "Reclaim must be disabled");
  check(!machine.useAfterFree(), "production discipline must not fault");
}

// Trace H — the TLC counterexample for the second premise, translated
// verbatim from `ProducerMarkReclaim.restamp.counterexample.cfg`:
//
//   State 2: WorkerBeginBatch({r2})   ticket = 1
//   State 3: SlotAdvance              nextSeqId 1 -> 2
//   State 4: WorkerStampMark(r2)      stamps 1, now stale
//   State 5: WorkerEndStamping
//   State 6: WorkerAppend             records land under seq 2, stamps say 1
//   State 7: AdvanceCompleted         completedSeqId -> 1
//   State 8: WorkerReleaseBatchRefs   pins gone; only the stamp protects it
//   State 9: SetDestroyPending(r2)
//   State 10: Reclaim(r2)             <- Invariant NoUseAfterFree is violated
//
// The stamp (1) is at or below the watermark (1) while the chunk that names
// the record (2) has not completed, so `gcArena` frees a record the encoder is
// still going to read.
void testTraceCounterexampleStaleTicketPrematureReclaim() {
  const std::array<Step, 9> steps{{
      {Action::WorkerBeginBatch, mask(kR2)},
      {Action::SlotAdvance},
      {Action::WorkerStampMark, kR2},
      {Action::WorkerEndStamping},
      {Action::WorkerAppend},
      {Action::AdvanceCompleted},
      {Action::WorkerReleaseBatchRefs},
      {Action::SetDestroyPending, kR2},
      {Action::Reclaim, kR2},
  }};
  const Machine machine = replay(PinDiscipline::Enforced, steps,
                                 "tlc-restamp-counterexample",
                                 RestampDiscipline::Removed);

  checkEq(machine.workerTicket(), u64{1}, "worker stamped the stale ticket");
  checkEq(machine.workerAppendSeqId(), u64{2}, "records landed under seq 2");
  checkEq(machine.lastUsedSeqId(kR2), u64{1}, "stamp is below the chunk seq");
  check(!machine.workerAppendCoveredByStamps(),
        "without the re-stamp the append is not covered by its stamps");
  check(machine.freed(kR2), "r2 was reclaimed while chunk 2 was pending");
  check(machine.useAfterFree(),
        "restamp counterexample must reproduce the premature reclaim");

  // The same trace under the production discipline cannot reach the append:
  // WorkerAppend is guarded on the ticket being current, so the worker must
  // re-stamp first. That single guard is the whole difference.
  Machine production(PinDiscipline::Enforced, RestampDiscipline::Enforced);
  check(production.step({Action::WorkerBeginBatch, mask(kR2)}), "begin batch");
  check(production.step({Action::SlotAdvance}), "force-publish");
  check(production.step({Action::WorkerStampMark, kR2}), "stale stamp");
  check(production.step({Action::WorkerEndStamping}), "end stamping");
  check(!production.step({Action::WorkerAppend}),
        "append must be disabled until the moved ticket is re-stamped");
  check(production.step({Action::WorkerRestamp}), "re-stamp enabled");
  check(production.step({Action::WorkerAppend}), "append enabled after restamp");
  check(production.workerAppendCoveredByStamps(), "append now covered");
}

// ===========================================================================
// Trace I..L — T2b: the binding capture off `CommandQueue::mutex_`
// ===========================================================================

// Trace I — the interleaving T2b exists for, and the one design §8's T2b row
// asks the model to include: the producer is mid-capture of the live chunk
// while the replay worker drives a reclaim of a record the chunk does NOT
// name. Under the mutex those two could not overlap at all; now they do, and
// the capture must still be sound.
//
// In production the two collide inside HandleArena rather than inside the
// queue: the capture holds the arena's shared lock per probe, `gcArena` takes
// its unique lock, and the slot container is a pointer-stable `std::deque`.
// The model's claim is the ordering one — the record being captured is not the
// record being freed, and cannot become it.
void testTraceCaptureInterleavesWithWorkerReclaim() {
  const std::array<Step, 16> steps{{
      // chunk 1 names both resources and completes normally.
      {Action::PinChunkResources, mask(kR1) | mask(kR2)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
      {Action::StampMark, kR2},
      {Action::CaptureRead, kR2},
      {Action::EndCommit},
      {Action::ReleasePins},
      // chunk 2 names only r1, and stamps it.
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      {Action::StampMark, kR1},
      // ... and now, BETWEEN r1's stamp and r1's capture, the worker drops its
      // inherited refs and the completion path reclaims r2.
      {Action::WorkerReleaseRefs},
      {Action::AdvanceCompleted},
      {Action::SetDestroyPending, kR2},
      {Action::Reclaim, kR2},
      // The capture then reads r1, concurrently with all of that.
      {Action::CaptureRead, kR1},
  }};
  Machine machine =
      replay(PinDiscipline::Enforced, steps, "capture-interleaves-with-reclaim");

  check(machine.freed(kR2), "the unnamed record was reclaimed mid-capture");
  check(!machine.freed(kR1), "the captured record stayed live");
  check(machine.noCaptureAfterFree(),
        "capture concurrent with an unrelated reclaim must stay sound");
  check(machine.stampsPrecedeCapture(), "capture followed its own stamp");
  check(!machine.useAfterFree(), "T2b interleaving must not fault");

  // The pin is what makes the difference between the two records, not the
  // mutex: r1 is chunk-named and therefore retainer-pinned, so the very first
  // step of r2's reclaim sequence is disabled for it.
  check(machine.pinned(kR1), "chunk-named record is pinned");
  check(!machine.step({Action::SetDestroyPending, kR1}),
        "SetDestroyPending must be disabled for the record being captured");
  check(!machine.step({Action::Reclaim, kR1}),
        "Reclaim must be disabled for the record being captured");
}

// Trace J — the capture needs no re-stamp analog. A `SlotAdvance` lands
// between the stamp and the capture (in production: another actor
// force-publishing the open writing slot while `commit_chunk` runs unlocked).
// It moves the ticket, which is exactly what obliges the STAMP side to re-read
// under the mutex — and it changes nothing the capture reads, because the
// capture copies producer-owned values rather than anything derived from a
// seq.
void testTraceCaptureNeedsNoRestampAnalog() {
  const std::array<Step, 4> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},           // commitSeqId = 1, nextSeqId -> 2
      {Action::StampMark, kR1},      // stamps 1
      {Action::SlotAdvance},         // nextSeqId -> 3, ticket now stale
  }};
  Machine machine =
      replay(PinDiscipline::Enforced, steps, "capture-needs-no-restamp");

  checkEq(machine.nextSeqId(), u64{3}, "a publish moved the seq mid-window");
  const u64 stampBeforeCapture = machine.lastUsedSeqId(kR1);
  check(machine.step({Action::CaptureRead, kR1}),
        "capture is still enabled after the seq moved");
  checkEq(machine.lastUsedSeqId(kR1), stampBeforeCapture,
          "capture reads no seq and writes no seq");
  check(machine.noCaptureAfterFree(), "capture stayed sound across the advance");
  check(!machine.useAfterFree(), "no fault from the moved ticket");

  // Contrast: the WORKER's stamp side does owe a re-stamp for the same event.
  // Same `SlotAdvance`, and its append is refused until the ticket is re-read.
  Machine worker(PinDiscipline::Enforced);
  check(worker.step({Action::WorkerBeginBatch, mask(kR2)}), "begin batch");
  check(worker.step({Action::SlotAdvance}), "force-publish");
  check(worker.step({Action::WorkerStampMark, kR2}), "stale stamp");
  check(worker.step({Action::WorkerEndStamping}), "end stamping");
  check(!worker.step({Action::WorkerAppend}),
        "the STAMP side is blocked by the same event the capture ignores");
}

// Trace K — stamps-before-capture, as an enabledness fact. The model refuses a
// capture of an unmarked record, which is the executable form of "an early
// capture has no repair": a too-low stamp is fixed by a monotone re-stamp, a
// snapshot published for a record whose watermark this commit has not raised
// is not fixable at all.
void testTraceCaptureRequiresItsOwnStamp() {
  Machine machine(PinDiscipline::Enforced);
  check(machine.step({Action::PinChunkResources, mask(kR1) | mask(kR2)}), "pin");
  check(machine.step({Action::BeginMark}), "begin mark");
  check(!machine.step({Action::CaptureRead, kR1}),
        "capture must be disabled before this commit's stamp");
  check(machine.step({Action::StampMark, kR1}), "stamp r1");
  check(machine.step({Action::CaptureRead, kR1}), "capture enabled after stamp");
  check(!machine.step({Action::CaptureRead, kR2}),
        "r2's capture is still gated on r2's own stamp");
  check(machine.stampsPrecedeCapture(), "ordering protocol holds");
  // EndCommit needs BOTH loops complete for every named record — the commit
  // cannot return having captured a subset.
  check(!machine.step({Action::EndCommit}), "EndCommit blocked on r2");
  check(machine.step({Action::StampMark, kR2}), "stamp r2");
  check(!machine.step({Action::EndCommit}), "EndCommit still blocked on capture");
  check(machine.step({Action::CaptureRead, kR2}), "capture r2");
  check(machine.step({Action::EndCommit}), "EndCommit enabled once both ran");
}

// Trace L — the TLC counterexample for the capture premise, translated
// verbatim from `ProducerMarkReclaim.capture.counterexample.cfg`:
//
//   State 2: PinChunkResources({r1})
//   State 3: BeginMark                commitSeqId = 1
//   State 4: SetDestroyPending(r1)    <- only reachable with the pin removed
//   State 5: Reclaim(r1)              lastUsedSeqId 0 <= completedSeqId 0
//   State 6: StampMark(r1)
//   State 7: CaptureRead(r1)          <- Invariant NoCaptureAfterFree violated
//
// This is `captureChunkBufferBinding` reading a `BufferRecord` whose
// HandleArena slot `gcArena` already released, and publishing it into the
// chunk's `bufferSnapshots` as a live backing. Note the mark and capture loops
// run two steps AFTER the free and re-check nothing — which is why the pin,
// not a liveness test, has to be the guarantee.
void testTraceCounterexampleCaptureAfterFree() {
  const std::array<Step, 6> steps{{
      {Action::PinChunkResources, mask(kR1)},
      {Action::BeginMark},
      {Action::SetDestroyPending, kR1},
      {Action::Reclaim, kR1},
      {Action::StampMark, kR1},
      {Action::CaptureRead, kR1},
  }};
  const Machine machine =
      replay(PinDiscipline::Removed, steps, "tlc-capture-counterexample");

  check(machine.freed(kR1), "r1 was reclaimed inside the commit window");
  check(!machine.noCaptureAfterFree(),
        "capture counterexample must violate the capture-side invariant");
  check(machine.useAfterFree(),
        "and the shared fault flag trips too — same axis, longer trace");

  // Under the production discipline the trace dies at its third step, which is
  // precisely what licenses the capture to run without the queue mutex.
  Machine production(PinDiscipline::Enforced);
  check(production.step({Action::PinChunkResources, mask(kR1)}), "pin enabled");
  check(production.step({Action::BeginMark}), "begin mark enabled");
  check(!production.step({Action::SetDestroyPending, kR1}),
        "the pin disables the capture counterexample's first bad step");
  check(production.step({Action::StampMark, kR1}), "stamp enabled");
  check(production.step({Action::CaptureRead, kR1}), "capture enabled");
  check(production.noCaptureAfterFree(), "production capture is sound");
}

}  // namespace

int main() {
  try {
    testCanReclaimRecordTruthTable();
    testMarkStampUpperTruthTable();
    testTraceHappyPathCommit();
    testTraceWorkerReclaimsDuringMarkWindow();
    testTraceRetainerPinBlocksDestroyPending();
    testTraceCounterexampleUseAfterFree();
    testTraceCounterexampleLateStampAndCapture();
    testTraceMapFastReadStaleButSound();
    testTraceWorkerRestampCoversAdvancedSlotSeq();
    testTraceCounterexampleStaleTicketPrematureReclaim();
    testTraceCaptureInterleavesWithWorkerReclaim();
    testTraceCaptureNeedsNoRestampAnalog();
    testTraceCaptureRequiresItsOwnStamp();
    testTraceCounterexampleCaptureAfterFree();
  } catch (const TestFailure& failure) {
    std::cerr << "producer_mark_reclaim_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "producer_mark_reclaim_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }

  std::cout << "producer_mark_reclaim_spec passed\n";
  return 0;
}
