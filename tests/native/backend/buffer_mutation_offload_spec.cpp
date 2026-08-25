// Managed buffer mutation offload shared-predicate + trace-conformance spec.
//
// Binds `specs/verification/tla/BufferMutationOffload.tla` to C++ through the
// shared pure predicates in
// `src/dxmt9/dxmt9_mutation_offload_predicates.hpp`. See
// `specs/backend/buffer-mutation-offload/{requirements,spec}.md`
// (R-BACK-44.1..44.6) and the R-BACK-43.6 evidence-stack procedure this
// repeats.
//
// Two sections:
//
//   1. Truth tables over the boundary cases of `admitsManagedMutationOffload`,
//      `isPlainWritableLock`, `fifoOrdinalPrecedes`,
//      `mutationBlocksChunkReplay`, `chunkBlocksMutationApply`,
//      `captureRevisionIsCurrent`, and `directReaderMustFence`. These pin the
//      predicates themselves.
//
//   2. Trace conformance. A miniature state machine mirrors the model's
//      variables and takes the model's actions as steps; every transition that
//      has a predicate in the model calls the SAME C++ predicate the
//      production code will call. Representative interleavings — including
//      both TLC counterexample traces verbatim — are hand-encoded as step
//      sequences and replayed.
//
// ---------------------------------------------------------------------------
// TRANSLATION CONVENTION (TLA action -> native Step)
// ---------------------------------------------------------------------------
// A TLC trace prints one action name per state transition, e.g.
//
//     State 2: <Reserve line 265, ... of module BufferMutationOffload>
//     State 3: <Rotate line 316, ... of module BufferMutationOffload>
//
// Each such line maps to exactly one `Step` below:
//
//   TLA action           native Step
//   -------------------  ---------------------------------------------------
//   Reserve(m)           {Action::Reserve, m}
//   AbortReserve(m)      {Action::AbortReserve, m}
//   Rotate(m, b)         {Action::Rotate, m, b}
//   CommitMutation(m)    {Action::CommitMutation, m}
//   CommitChunk(c)       {Action::CommitChunk, c}
//   ReplayChunk(c)       {Action::ReplayChunk, c}
//   ApplyMutation(m)     {Action::ApplyMutation, m}
//   EncodeChunk(c)       {Action::EncodeChunk, c}
//
// TLC prints identifiers as 1-based tuple positions (`<<"Committed","Idle">>`)
// and model values (`b1`); the native machine uses 0-based indices, so `c1` is
// `kC1 == 0` and `b1` is `kB1 == 0`.
//
// Rules that make the replay meaningful:
//
//   * `Machine::step` returns false when the model's action guard is not
//     enabled. A trace whose guards do not all hold is a translation error,
//     not a finding — `replay()` fails the test on it.
//   * `Machine::staleEncodeRead` / `staleSnapshot` mirror the model's two
//     sticky fault flags and are set at exactly the sites the model sets them
//     (EncodeChunk on a backing that does not hold the captured revision;
//     CommitChunk on a record revision behind the app-published one).
//   * `FifoDiscipline` and `RotationDiscipline` are constructor arguments,
//     matching the model constants. `Enforced` / `Synchronous` is production;
//     `Removed` / `Deferred` are the corresponding counterexample worlds. They
//     are independent: each has its own `.cfg` and its own translated trace
//     below.
//
// A green production model plus two red counterexample models plus these
// replays mean the model and the code agree on the transition vocabulary. It
// does NOT mean the offload mode is implemented — it is not
// (`specs/backend/buffer-mutation-offload/gap.md`) — nor that Metal, byte
// layout, or the staged-byte budget behave as assumed.

#include "../../../src/dxmt9/dxmt9_mutation_offload_predicates.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace mo = dxmt9::resources::mutation_offload;

using mo::admitsManagedMutationOffload;
using mo::captureRevisionIsCurrent;
using mo::chunkBlocksMutationApply;
using mo::directReaderMustFence;
using mo::fifoOrdinalPrecedes;
using mo::isPlainWritableLock;
using mo::mutationBlocksChunkReplay;

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

constexpr std::uint32_t kPoolDefault = 0u;
constexpr std::uint32_t kPoolManaged = mo::kPoolManaged;
constexpr std::uint32_t kPoolSystemMem = 2u;
constexpr std::uint32_t kPoolScratch = 3u;

constexpr std::uint32_t kNoFlags = 0u;
constexpr std::uint32_t kReadOnly = mo::kLockReadOnly;
constexpr std::uint32_t kNoOverwrite = mo::kLockNoOverwrite;
constexpr std::uint32_t kDiscard = mo::kLockDiscard;
// A bit the predicate must be blind to: `D3DLOCK_DONOTWAIT` (0x00004000).
// R-BACK-44.1 excludes three named classes, not "anything unusual", so an
// unrelated flag must not change admission.
constexpr std::uint32_t kDoNotWait = 0x00004000u;

// TLA+: the lock-class half of BufferMutationOffload!Reserve's enabling
// condition. Exhaustive over the three exclusion bits.
void testPlainWritableLockTruthTable() {
  check(isPlainWritableLock(kNoFlags), "bare writable lock is plain");

  // Each exclusion bit alone disqualifies.
  check(!isPlainWritableLock(kReadOnly), "READONLY is not a writable lock");
  check(!isPlainWritableLock(kNoOverwrite), "NOOVERWRITE is not plain");
  check(!isPlainWritableLock(kDiscard), "DISCARD is not plain");

  // Every pair and the triple, so no combination sneaks through.
  check(!isPlainWritableLock(kReadOnly | kNoOverwrite), "READONLY+NOOVERWRITE");
  check(!isPlainWritableLock(kReadOnly | kDiscard), "READONLY+DISCARD");
  check(!isPlainWritableLock(kNoOverwrite | kDiscard), "NOOVERWRITE+DISCARD");
  check(!isPlainWritableLock(kReadOnly | kNoOverwrite | kDiscard), "all three");

  // Unrelated bits are transparent, in isolation and beside an exclusion.
  check(isPlainWritableLock(kDoNotWait), "DONOTWAIT alone stays plain");
  check(!isPlainWritableLock(kDoNotWait | kDiscard),
        "DONOTWAIT does not rescue DISCARD");

  // The exact bit values matter: these are mirrored from
  // `d3d9_pe_buffer_hazard.hpp`, and a drift there is a silent scope change.
  checkEq(kReadOnly, std::uint32_t{0x00000010u}, "D3DLOCK_READONLY bit");
  checkEq(kNoOverwrite, std::uint32_t{0x00001000u}, "D3DLOCK_NOOVERWRITE bit");
  checkEq(kDiscard, std::uint32_t{0x00002000u}, "D3DLOCK_DISCARD bit");
  checkEq(kPoolManaged, std::uint32_t{1u}, "D3DPOOL_MANAGED value");
}

// TLA+: BufferMutationOffload!Reserve's enabling condition in full —
// R-BACK-44.1's scope gate. Every conjunct is checked as individually
// load-bearing: with the other four satisfied, flipping this one must flip the
// answer.
void testAdmissionTruthTable() {
  // The one admitted shape.
  check(admitsManagedMutationOffload(true, true, kPoolManaged, kNoFlags, true),
        "plain Managed writable unlock with a versioned backing is admitted");

  // Conjunct 1: the mode itself. R-BACK-44.1's rollback must be byte-identical
  // to the synchronous path, which starts with never taking this branch.
  check(!admitsManagedMutationOffload(false, true, kPoolManaged, kNoFlags, true),
        "mode off is never admitted");

  // Conjunct 2: inline replay has no worker to carry the task, so the mode
  // must resolve to off even when the env var asked for it.
  check(!admitsManagedMutationOffload(true, false, kPoolManaged, kNoFlags, true),
        "inline replay is never admitted");

  // Conjunct 3: pool. Every other D3DPOOL keeps its existing path.
  check(!admitsManagedMutationOffload(true, true, kPoolDefault, kNoFlags, true),
        "DEFAULT pool is out of scope");
  check(!admitsManagedMutationOffload(true, true, kPoolSystemMem, kNoFlags, true),
        "SYSTEMMEM pool is out of scope");
  check(!admitsManagedMutationOffload(true, true, kPoolScratch, kNoFlags, true),
        "SCRATCH pool is out of scope");

  // Conjunct 4: lock class. Same three exclusions as above, now through the
  // full gate, because the reason each is excluded is an offload-ordering
  // reason and not a lock-validity one.
  check(!admitsManagedMutationOffload(true, true, kPoolManaged, kReadOnly, true),
        "READONLY unlock has nothing to offload");
  check(!admitsManagedMutationOffload(true, true, kPoolManaged, kNoOverwrite, true),
        "NOOVERWRITE skips the PE hazard seal");
  check(!admitsManagedMutationOffload(true, true, kPoolManaged, kDiscard, true),
        "DISCARD zero-fills beyond the staged span");
  check(!admitsManagedMutationOffload(true, true, kPoolManaged,
                                      kNoOverwrite | kDiscard, true),
        "a combined excluded lock class stays excluded");
  check(admitsManagedMutationOffload(true, true, kPoolManaged, kDoNotWait, true),
        "an unrelated lock flag does not narrow the scope");

  // Conjunct 5: the record must have a rename ring to rotate.
  check(!admitsManagedMutationOffload(true, true, kPoolManaged, kNoFlags, false),
        "non-versioned backing has nothing to rotate");

  // And the combinations that fail for more than one reason still fail.
  check(!admitsManagedMutationOffload(false, false, kPoolDefault,
                                      kReadOnly, false),
        "nothing admitted when every conjunct is false");
}

// TLA+: BufferMutationOffload!FifoOrdinalPrecedes. Strict, and closed against
// the unreserved sentinel from both sides.
void testFifoOrdinalPrecedesTruthTable() {
  // Adjacency and equality — the two boundaries the ordering guards sit on.
  check(fifoOrdinalPrecedes(1, 2), "adjacent ordinals precede");
  check(!fifoOrdinalPrecedes(2, 1), "the reverse pair does not");
  check(!fifoOrdinalPrecedes(2, 2), "equal ordinals do not precede");
  check(fifoOrdinalPrecedes(1, 9), "distant ordinals precede");

  // Sentinel: an item holding no reservation imposes no order, and cannot be
  // ordered against. Both argument positions, so a zero can never be mistaken
  // for a head-of-queue position.
  check(!fifoOrdinalPrecedes(mo::kUnreservedOrdinal, 1),
        "an unreserved item does not precede a reserved one");
  check(!fifoOrdinalPrecedes(1, mo::kUnreservedOrdinal),
        "a reserved item does not precede an unreserved one");
  check(!fifoOrdinalPrecedes(mo::kUnreservedOrdinal, mo::kUnreservedOrdinal),
        "two unreserved items are unordered");
  checkEq(mo::kUnreservedOrdinal, u64{0}, "the unreserved sentinel is zero");

  // Saturation ends.
  const u64 maxOrdinal = ~u64{0};
  check(fifoOrdinalPrecedes(maxOrdinal - 1, maxOrdinal), "top of the domain");
  check(!fifoOrdinalPrecedes(maxOrdinal, maxOrdinal - 1), "and its reverse");
}

// TLA+: BufferMutationOffload!MutationBlocksChunkReplay and
// !ChunkBlocksMutationApply — the two halves of R-BACK-44.3.
void testFifoBlockingTruthTable() {
  // ReplayChunk half. Ordinal 1 mutation, ordinal 2 chunk: the mutation must
  // land first, and only while it is unapplied.
  check(mutationBlocksChunkReplay(1, /*applied=*/false, 2),
        "an unapplied earlier mutation blocks the chunk");
  check(!mutationBlocksChunkReplay(1, /*applied=*/true, 2),
        "an applied earlier mutation does not");
  check(!mutationBlocksChunkReplay(3, /*applied=*/false, 2),
        "a later mutation never blocks an earlier chunk");
  check(!mutationBlocksChunkReplay(2, /*applied=*/false, 2),
        "equal ordinals cannot happen and do not block");
  check(!mutationBlocksChunkReplay(mo::kUnreservedOrdinal, false, 2),
        "an unreserved mutation blocks nothing");

  // ApplyMutation half — the mirror obligation, and a different one: this
  // keeps the mutation from overwriting bytes a pending chunk still reads.
  check(chunkBlocksMutationApply(1, /*replayed=*/false, 2),
        "an unreplayed earlier chunk blocks the mutation");
  check(!chunkBlocksMutationApply(1, /*replayed=*/true, 2),
        "a replayed earlier chunk does not");
  check(!chunkBlocksMutationApply(3, /*replayed=*/false, 2),
        "a later chunk never blocks an earlier mutation");
  check(!chunkBlocksMutationApply(2, /*replayed=*/false, 2),
        "equal ordinals do not block");
  check(!chunkBlocksMutationApply(mo::kUnreservedOrdinal, false, 2),
        "an unreserved chunk blocks nothing");

  // The two are not one function with swapped arguments. Each consults the
  // progress flag of ITS OWN item, so on the same ordinal pair they disagree
  // whenever those flags do — substituting one for the other is a real bug,
  // not a refactor.
  check(!mutationBlocksChunkReplay(1, /*applied=*/true, 2) &&
            chunkBlocksMutationApply(1, /*replayed=*/false, 2),
        "each half reads its own item's progress, not a shared one");
  check(mutationBlocksChunkReplay(1, /*applied=*/false, 2) &&
            !chunkBlocksMutationApply(1, /*replayed=*/true, 2),
        "and the same holds with the two flags exchanged");
}

// TLA+: BufferMutationOffload!CaptureRevisionIsCurrent — R-BACK-44.2 step 2.
void testCaptureRevisionTruthTable() {
  checkEq(captureRevisionIsCurrent(0, 0), true, "origin capture is current");
  checkEq(captureRevisionIsCurrent(3, 3), true, "matched capture is current");

  // Behind: the deferred-rotation failure. The capture froze a revision the
  // app has already superseded.
  checkEq(captureRevisionIsCurrent(2, 3), false, "capture one revision behind");
  checkEq(captureRevisionIsCurrent(0, 1), false, "capture behind from origin");

  // Ahead: not a shape production can reach, and deliberately still false —
  // the predicate is equality, not `>=`, so a capture that ran ahead of the
  // published revision is reported rather than silently accepted.
  checkEq(captureRevisionIsCurrent(4, 3), false, "capture ahead is not current");

  const u64 maxRevision = ~u64{0};
  checkEq(captureRevisionIsCurrent(maxRevision, maxRevision), true,
          "saturated revisions compare equal");
  checkEq(captureRevisionIsCurrent(maxRevision - 1, maxRevision), false,
          "and one short does not");
}

// TLA+: ReplayScopedDrain!ScopedReturnSafe (R-BACK-2.51(d)(i)), the fence
// R-BACK-44.5 extends to mutation tasks.
void testDirectReaderFenceTruthTable() {
  check(!directReaderMustFence(0, 0), "nothing queued, nothing to wait for");
  check(!directReaderMustFence(3, 3), "caught up exactly: no wait");
  check(directReaderMustFence(4, 3), "one task behind: wait");
  check(directReaderMustFence(9, 0), "far behind: wait");
  // The replayed watermark never legitimately exceeds the queued one
  // (`ReplayedLeQueued`), and if it somehow did, that is not a reason to wait.
  check(!directReaderMustFence(3, 4), "replayed ahead of queued: no wait");
}

// ===========================================================================
// 2. Trace conformance
// ===========================================================================

// Matches the production configuration's CONSTANTS.
constexpr std::size_t kChunks = 3;      // NumChunks = 3
constexpr std::size_t kMutations = 2;   // NumMutations = 2
constexpr std::size_t kBackings = 3;    // Backings = {b1, b2, b3}
constexpr u64 kMaxOrdinal = 6;          // MAX_ORDINAL = 6
// Mirrors CONSTANT NoBacking: one past the last real ring entry.
constexpr std::size_t kNoBacking = kBackings;

constexpr std::size_t kC1 = 0, kC2 = 1;
constexpr std::size_t kM1 = 0, kM2 = 1;
constexpr std::size_t kB1 = 0, kB2 = 1;

enum class FifoDiscipline { Enforced, Removed };
enum class RotationDiscipline { Synchronous, Deferred };

// Mirrors BufferMutationOffload!ChunkPhases.
enum class ChunkPhase { Idle, Committed, Replayed, Encoded };

// Mirrors BufferMutationOffload!MutationPhases.
enum class MutationPhase { Idle, Reserved, Rotated, Committed, Applied };

enum class Action {
  Reserve,
  AbortReserve,
  Rotate,
  CommitMutation,
  CommitChunk,
  ReplayChunk,
  ApplyMutation,
  EncodeChunk,
};

struct Step {
  Action action{};
  std::size_t item = 0;     // chunk index or mutation index
  std::size_t backing = 0;  // Rotate / deferred ApplyMutation target only
};

const char* actionName(Action action) {
  switch (action) {
    case Action::Reserve:        return "Reserve";
    case Action::AbortReserve:   return "AbortReserve";
    case Action::Rotate:         return "Rotate";
    case Action::CommitMutation: return "CommitMutation";
    case Action::CommitChunk:    return "CommitChunk";
    case Action::ReplayChunk:    return "ReplayChunk";
    case Action::ApplyMutation:  return "ApplyMutation";
    case Action::EncodeChunk:    return "EncodeChunk";
  }
  return "<unknown>";
}

// One-to-one with the model's VARIABLES. Transitions the model expresses
// through the shared predicate vocabulary call the shared C++ predicates.
class Machine {
 public:
  explicit Machine(FifoDiscipline fifo = FifoDiscipline::Enforced,
                   RotationDiscipline rotation = RotationDiscipline::Synchronous)
      : fifo_(fifo), rotation_(rotation) {}

  bool staleEncodeRead() const noexcept { return staleEncodeRead_; }
  bool staleSnapshot() const noexcept { return staleSnapshot_; }
  u64 recordRevision() const noexcept { return recordRevision_; }
  u64 logicalRevision() const noexcept { return logicalRevision_; }
  std::size_t recordBacking() const noexcept { return recordBacking_; }
  u64 nextOrdinal() const noexcept { return nextOrdinal_; }
  u64 backingContent(std::size_t b) const noexcept { return backingContent_[b]; }
  ChunkPhase chunkPhase(std::size_t c) const noexcept { return chunkPhase_[c]; }
  u64 chunkOrdinal(std::size_t c) const noexcept { return chunkOrdinal_[c]; }
  std::size_t capturedBacking(std::size_t c) const noexcept {
    return capturedBacking_[c];
  }
  u64 capturedRevision(std::size_t c) const noexcept {
    return capturedRevision_[c];
  }
  MutationPhase mutationPhase(std::size_t m) const noexcept {
    return mutationPhase_[m];
  }
  u64 mutationOrdinal(std::size_t m) const noexcept { return mutationOrdinal_[m]; }
  std::size_t mutationTarget(std::size_t m) const noexcept {
    return mutationTarget_[m];
  }
  u64 mutationRevision(std::size_t m) const noexcept {
    return mutationRevision_[m];
  }

  // TLA+: BufferMutationOffload!EncodeReadsAppliedBytes.
  bool encodeReadsAppliedBytes() const noexcept { return !staleEncodeRead_; }

  // TLA+: BufferMutationOffload!SnapshotRevisionIsCurrent.
  bool snapshotRevisionIsCurrent() const noexcept { return !staleSnapshot_; }

  // TLA+: BufferMutationOffload!NoLiveCaptureOverwritten — the positional form
  // of the reuse rule this model imports from BufferBackingVersioning.
  bool noLiveCaptureOverwritten() const noexcept {
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (mutationPhase_[m] != MutationPhase::Applied) continue;
      for (std::size_t c = 0; c < kChunks; ++c) {
        if (!chunkLive(c)) continue;
        if (!fifoOrdinalPrecedes(chunkOrdinal_[c], mutationOrdinal_[m])) continue;
        if (capturedBacking_[c] == mutationTarget_[m]) return false;
      }
    }
    return true;
  }

  // TLA+: BufferMutationOffload!FifoApplicationOrder — R-BACK-44.3 stated
  // directly, both directions over the one shared ordinal domain.
  bool fifoApplicationOrder() const noexcept {
    for (std::size_t c = 0; c < kChunks; ++c) {
      if (!chunkReplayed(c)) continue;
      for (std::size_t m = 0; m < kMutations; ++m) {
        if (mutationBlocksChunkReplay(mutationOrdinal_[m], mutationApplied(m),
                                      chunkOrdinal_[c])) {
          return false;
        }
      }
    }
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (!mutationApplied(m)) continue;
      for (std::size_t c = 0; c < kChunks; ++c) {
        if (chunkBlocksMutationApply(chunkOrdinal_[c], chunkReplayed(c),
                                     mutationOrdinal_[m])) {
          return false;
        }
      }
    }
    return true;
  }

  // TLA+: BufferMutationOffload!PublishedRevisionCount — the checkable half of
  // "a rejected reservation published nothing".
  bool publishedRevisionCount() const noexcept {
    u64 committed = 0;
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (mutationPhase_[m] == MutationPhase::Committed ||
          mutationPhase_[m] == MutationPhase::Applied) {
        committed += 1;
      }
    }
    return logicalRevision_ == committed;
  }

  // TLA+: BufferMutationOffload!RecordRevisionCountsRotations.
  bool recordRevisionCountsRotations() const noexcept {
    u64 rotations = 0;
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (rotationDone(m)) rotations += 1;
    }
    return recordRevision_ == rotations;
  }

  // TLA+: BufferMutationOffload!TaskCarriesRotatedTarget — R-BACK-44.2a.
  bool taskCarriesRotatedTarget() const noexcept {
    if (rotation_ != RotationDiscipline::Synchronous) return true;
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (mutationPhase_[m] != MutationPhase::Committed &&
          mutationPhase_[m] != MutationPhase::Applied) {
        continue;
      }
      if (mutationTarget_[m] >= kBackings) return false;
      if (mutationRevision_[m] == 0) return false;
    }
    return true;
  }

  // Every safety invariant the production configuration lists, in one call.
  bool safety() const noexcept {
    return encodeReadsAppliedBytes() && snapshotRevisionIsCurrent() &&
           noLiveCaptureOverwritten() && fifoApplicationOrder() &&
           publishedRevisionCount() && recordRevisionCountsRotations() &&
           taskCarriesRotatedTarget();
  }

  // Returns false when the model's guard for `step` is not enabled — i.e. the
  // encoded trace is not a behaviour of the spec.
  bool step(const Step& s);

 private:
  bool chunkCommitted(std::size_t c) const noexcept {
    return chunkPhase_[c] != ChunkPhase::Idle;
  }
  bool chunkReplayed(std::size_t c) const noexcept {
    return chunkPhase_[c] == ChunkPhase::Replayed ||
           chunkPhase_[c] == ChunkPhase::Encoded;
  }
  bool chunkLive(std::size_t c) const noexcept {
    return chunkPhase_[c] == ChunkPhase::Committed ||
           chunkPhase_[c] == ChunkPhase::Replayed;
  }
  bool mutationApplied(std::size_t m) const noexcept {
    return mutationPhase_[m] == MutationPhase::Applied;
  }
  // TLA+: BufferMutationOffload!ProducerBusy — the one app thread is inside an
  // unlock transaction, so no chunk commit can interleave with it.
  bool producerBusy() const noexcept {
    for (std::size_t m = 0; m < kMutations; ++m) {
      if (mutationPhase_[m] == MutationPhase::Reserved ||
          mutationPhase_[m] == MutationPhase::Rotated) {
        return true;
      }
    }
    return false;
  }
  // TLA+: BufferMutationOffload!BackingFreeForRotation — the R-BACK-5.11
  // selection gate, imported in its encode-scoped form.
  bool backingFreeForRotation(std::size_t b) const noexcept {
    for (std::size_t c = 0; c < kChunks; ++c) {
      if (chunkLive(c) && capturedBacking_[c] == b) return false;
    }
    return true;
  }
  // TLA+: BufferMutationOffload!RotationDone.
  bool rotationDone(std::size_t m) const noexcept {
    if (rotation_ == RotationDiscipline::Synchronous) {
      return mutationPhase_[m] == MutationPhase::Rotated ||
             mutationPhase_[m] == MutationPhase::Committed ||
             mutationPhase_[m] == MutationPhase::Applied;
    }
    return mutationPhase_[m] == MutationPhase::Applied;
  }

  FifoDiscipline fifo_;
  RotationDiscipline rotation_;
  std::size_t recordBacking_ = kB1;
  u64 recordRevision_ = 0;
  u64 logicalRevision_ = 0;
  std::array<u64, kBackings> backingContent_{};
  u64 nextOrdinal_ = 1;
  std::array<ChunkPhase, kChunks> chunkPhase_{};
  std::array<u64, kChunks> chunkOrdinal_{};
  std::array<std::size_t, kChunks> capturedBacking_{
      {kNoBacking, kNoBacking, kNoBacking}};
  std::array<u64, kChunks> capturedRevision_{};
  std::array<MutationPhase, kMutations> mutationPhase_{};
  std::array<u64, kMutations> mutationOrdinal_{};
  std::array<std::size_t, kMutations> mutationTarget_{{kNoBacking, kNoBacking}};
  std::array<u64, kMutations> mutationRevision_{};
  bool staleEncodeRead_ = false;
  bool staleSnapshot_ = false;
};

bool Machine::step(const Step& s) {
  switch (s.action) {
    case Action::Reserve: {
      // TLA+: Reserve(m) — R-BACK-44.2 step 1. The ordinal is taken FIRST, and
      // it is the only fallible part of the transaction.
      const std::size_t m = s.item;
      if (m >= kMutations) return false;
      if (mutationPhase_[m] != MutationPhase::Idle) return false;
      if (producerBusy()) return false;
      for (std::size_t m2 = 0; m2 < m; ++m2) {
        if (mutationPhase_[m2] == MutationPhase::Idle) return false;
      }
      if (nextOrdinal_ > kMaxOrdinal) return false;
      mutationOrdinal_[m] = nextOrdinal_;
      nextOrdinal_ += 1;
      mutationPhase_[m] = MutationPhase::Reserved;
      return true;
    }
    case Action::AbortReserve: {
      // TLA+: AbortReserve(m) — the retryable pre-effect rejection. The
      // position is burned, the budget returns, and nothing visible moved.
      const std::size_t m = s.item;
      if (m >= kMutations) return false;
      if (mutationPhase_[m] != MutationPhase::Reserved) return false;
      mutationPhase_[m] = MutationPhase::Idle;
      mutationOrdinal_[m] = mo::kUnreservedOrdinal;
      return true;
    }
    case Action::Rotate: {
      // TLA+: Rotate(m, b) — R-BACK-44.2 step 2, THE PREMISE. Note what it
      // does not touch: `backingContent_`. That is the offloaded byte motion.
      const std::size_t m = s.item;
      const std::size_t b = s.backing;
      if (rotation_ != RotationDiscipline::Synchronous) return false;
      if (m >= kMutations || b >= kBackings) return false;
      if (mutationPhase_[m] != MutationPhase::Reserved) return false;
      if (!backingFreeForRotation(b)) return false;
      if (recordRevision_ >= kMutations) return false;
      recordBacking_ = b;
      recordRevision_ += 1;
      mutationTarget_[m] = b;
      mutationRevision_[m] = recordRevision_;
      mutationPhase_[m] = MutationPhase::Rotated;
      return true;
    }
    case Action::CommitMutation: {
      // TLA+: CommitMutation(m) — R-BACK-44.2 step 3, infallible. This is the
      // point the app's Unlock publishes new content.
      const std::size_t m = s.item;
      if (m >= kMutations) return false;
      const MutationPhase required = rotation_ == RotationDiscipline::Synchronous
                                         ? MutationPhase::Rotated
                                         : MutationPhase::Reserved;
      if (mutationPhase_[m] != required) return false;
      if (logicalRevision_ >= kMutations) return false;
      mutationPhase_[m] = MutationPhase::Committed;
      logicalRevision_ += 1;
      return true;
    }
    case Action::CommitChunk: {
      // TLA+: CommitChunk(c) — `Pool::captureChunkBufferBinding` freezing the
      // live record. The shared predicate is the fault test.
      const std::size_t c = s.item;
      if (c >= kChunks) return false;
      if (chunkPhase_[c] != ChunkPhase::Idle) return false;
      for (std::size_t c2 = 0; c2 < c; ++c2) {
        if (chunkPhase_[c2] == ChunkPhase::Idle) return false;
      }
      if (producerBusy()) return false;
      if (nextOrdinal_ > kMaxOrdinal) return false;
      chunkOrdinal_[c] = nextOrdinal_;
      nextOrdinal_ += 1;
      capturedBacking_[c] = recordBacking_;
      capturedRevision_[c] = recordRevision_;
      chunkPhase_[c] = ChunkPhase::Committed;
      staleSnapshot_ = staleSnapshot_ ||
                       !captureRevisionIsCurrent(recordRevision_, logicalRevision_);
      return true;
    }
    case Action::ReplayChunk: {
      // TLA+: ReplayChunk(c). Chunks are FIFO among themselves in every
      // configuration; the mutation conjunct is the one FifoDiscipline drops.
      const std::size_t c = s.item;
      if (c >= kChunks) return false;
      if (chunkPhase_[c] != ChunkPhase::Committed) return false;
      for (std::size_t c2 = 0; c2 < kChunks; ++c2) {
        if (fifoOrdinalPrecedes(chunkOrdinal_[c2], chunkOrdinal_[c]) &&
            !chunkReplayed(c2)) {
          return false;
        }
      }
      if (fifo_ == FifoDiscipline::Enforced) {
        for (std::size_t m = 0; m < kMutations; ++m) {
          if (mutationBlocksChunkReplay(mutationOrdinal_[m], mutationApplied(m),
                                        chunkOrdinal_[c])) {
            return false;
          }
        }
      }
      chunkPhase_[c] = ChunkPhase::Replayed;
      return true;
    }
    case Action::ApplyMutation: {
      // TLA+: ApplyMutation(m) — R-BACK-44.3. Mutations stay FIFO among
      // themselves in every configuration, so FifoDiscipline deletes exactly
      // one premise and no more.
      const std::size_t m = s.item;
      if (m >= kMutations) return false;
      if (mutationPhase_[m] != MutationPhase::Committed) return false;
      for (std::size_t m2 = 0; m2 < kMutations; ++m2) {
        if (fifoOrdinalPrecedes(mutationOrdinal_[m2], mutationOrdinal_[m]) &&
            !mutationApplied(m2)) {
          return false;
        }
      }
      if (fifo_ == FifoDiscipline::Enforced) {
        for (std::size_t c = 0; c < kChunks; ++c) {
          if (chunkBlocksMutationApply(chunkOrdinal_[c], chunkReplayed(c),
                                       mutationOrdinal_[m])) {
            return false;
          }
        }
      }
      if (rotation_ == RotationDiscipline::Synchronous) {
        // The worker applies to the LEASE (R-BACK-44.2a), never to the
        // record's then-current active backing.
        backingContent_[mutationTarget_[m]] = mutationRevision_[m];
      } else {
        const std::size_t b = s.backing;
        if (b >= kBackings) return false;
        if (!backingFreeForRotation(b)) return false;
        if (recordRevision_ >= kMutations) return false;
        recordBacking_ = b;
        recordRevision_ += 1;
        mutationTarget_[m] = b;
        mutationRevision_[m] = recordRevision_;
        backingContent_[b] = recordRevision_;
      }
      mutationPhase_[m] = MutationPhase::Applied;
      return true;
    }
    case Action::EncodeChunk: {
      // TLA+: EncodeChunk(c). Its only ordering premise is its OWN chunk's
      // replay — R-BACK-44.4a is exactly the fact that encode of an earlier
      // chunk may run after a later mutation has been applied.
      const std::size_t c = s.item;
      if (c >= kChunks) return false;
      if (chunkPhase_[c] != ChunkPhase::Replayed) return false;
      chunkPhase_[c] = ChunkPhase::Encoded;
      staleEncodeRead_ = staleEncodeRead_ ||
                         backingContent_[capturedBacking_[c]] != capturedRevision_[c];
      return true;
    }
  }
  return false;
}

// Replays a translated TLC trace. A step whose guard is not enabled means the
// translation is wrong, so it fails the test rather than being ignored.
template <std::size_t N>
Machine replay(const std::array<Step, N>& steps, std::string_view traceName,
               FifoDiscipline fifo = FifoDiscipline::Enforced,
               RotationDiscipline rotation = RotationDiscipline::Synchronous) {
  Machine machine(fifo, rotation);
  for (std::size_t i = 0; i < N; ++i) {
    if (!machine.step(steps[i])) {
      std::ostringstream out;
      out << traceName << ": step " << i << " (" << actionName(steps[i].action)
          << ", item=" << steps[i].item << ", backing=" << steps[i].backing
          << ") was not enabled";
      fail(out.str());
    }
  }
  return machine;
}

// Trace A — the production happy path, and the sequence diagram in
// `spec.md` §4 read as steps: chunk A commits (capturing the pre-rotation
// backing), the unlock reserves / rotates / commits, chunk B commits
// (capturing the post-rotation backing), and the worker drains the one queue
// in order. Both chunks then encode to the content they captured.
void testTraceProductionOrdering() {
  const std::array<Step, 10> steps{{
      {Action::CommitChunk, kC1},              // ordinal 1, captures (b1, rev0)
      {Action::Reserve, kM1},                  // ordinal 2
      {Action::Rotate, kM1, kB2},              // b1 is live-captured by c1
      {Action::CommitMutation, kM1},           // unlock returns; published = 1
      {Action::CommitChunk, kC2},              // ordinal 3, captures (b2, rev1)
      {Action::ReplayChunk, kC1},              // FIFO position 1
      {Action::ApplyMutation, kM1},            // FIFO position 2
      {Action::ReplayChunk, kC2},              // FIFO position 3
      {Action::EncodeChunk, kC1},
      {Action::EncodeChunk, kC2},
  }};
  const Machine machine = replay(steps, "production-ordering");

  check(machine.safety(), "the production interleaving must satisfy safety");
  checkEq(machine.capturedBacking(kC1), kB1, "c1 captured the old backing");
  checkEq(machine.capturedRevision(kC1), u64{0}, "c1 captured revision 0");
  checkEq(machine.capturedBacking(kC2), kB2, "c2 captured the rotated backing");
  checkEq(machine.capturedRevision(kC2), u64{1}, "c2 captured revision 1");
  // The mutation never touched the backing c1 captured — `spec.md` §2's
  // "rotation preserves prior ring backings".
  checkEq(machine.backingContent(kB1), u64{0}, "c1's backing was not rewritten");
  checkEq(machine.backingContent(kB2), u64{1}, "c2's backing holds the new bytes");
}

// Trace B — R-BACK-44.4a, the reason encode carries no cross-chunk order. An
// EARLIER chunk is encoded AFTER a later mutation has been applied, which is
// the interleaving that makes encode-side snapshot sourcing a prerequisite
// rather than a nicety. It is sound here because the mutation could not target
// the backing that still-live chunk captured.
void testTraceEncodeDecoupledFromMutation() {
  const std::array<Step, 9> steps{{
      {Action::CommitChunk, kC1},     // ordinal 1, captures (b1, rev0)
      {Action::ReplayChunk, kC1},     // replayed, but NOT encoded
      {Action::Reserve, kM1},         // ordinal 2
      {Action::Rotate, kM1, kB2},
      {Action::CommitMutation, kM1},
      {Action::CommitChunk, kC2},     // ordinal 3, captures (b2, rev1)
      {Action::ApplyMutation, kM1},   // applies while c1 is still unencoded
      {Action::ReplayChunk, kC2},
      {Action::EncodeChunk, kC2},     // the LATER chunk encodes first
  }};
  Machine machine = replay(steps, "encode-decoupled");

  check(machine.safety(), "the decoupled interleaving must satisfy safety");
  // Now the earlier chunk encodes, after the later mutation already landed.
  check(machine.step({Action::EncodeChunk, kC1}), "late encode of c1 enabled");
  check(machine.encodeReadsAppliedBytes(),
        "a late encode of an earlier chunk still reads its captured content");
  checkEq(machine.backingContent(kB1), u64{0}, "its backing was never rewritten");

  // The gate is what makes that true, and it is checkable by enabledness: a
  // rotation onto c1's captured backing is refused while c1 is live.
  Machine gated;
  check(gated.step({Action::CommitChunk, kC1}), "commit c1");
  check(gated.step({Action::Reserve, kM1}), "reserve");
  check(!gated.step({Action::Rotate, kM1, kB1}),
        "rotation onto a live chunk's captured backing must be disabled");
  check(gated.step({Action::Rotate, kM1, kB2}), "a free entry is admissible");
}

// Trace C — R-BACK-44.2's transactional order, as enabledness facts. Reserve
// must precede rotate, rotate must precede commit, and a chunk commit cannot
// land inside the transaction (one app thread).
void testTraceTransactionOrder() {
  Machine machine;
  // Rotation before the reservation is not a step at all: the ordinal must be
  // fixed before anything externally visible happens.
  check(!machine.step({Action::Rotate, kM1, kB2}),
        "Rotate must be disabled before Reserve");
  check(!machine.step({Action::CommitMutation, kM1}),
        "CommitMutation must be disabled before Reserve");

  check(machine.step({Action::Reserve, kM1}), "reserve enabled");
  checkEq(machine.mutationOrdinal(kM1), u64{1}, "reserve fixed ordinal 1");
  checkEq(machine.recordRevision(), u64{0}, "reserve published nothing");

  // The commit step is not fallible, but it is not reachable either until the
  // rotation has run: everything fallible happens before step 2.
  check(!machine.step({Action::CommitMutation, kM1}),
        "CommitMutation must be disabled before Rotate");
  // And the app cannot commit a chunk from inside its own Unlock.
  check(!machine.step({Action::CommitChunk, kC1}),
        "CommitChunk must be disabled inside the unlock transaction");

  check(machine.step({Action::Rotate, kM1, kB2}), "rotate enabled");
  checkEq(machine.recordRevision(), u64{1}, "rotation bumped the revision");
  checkEq(machine.recordBacking(), kB2, "rotation published the new entry");
  checkEq(machine.mutationTarget(kM1), kB2, "the task leased that entry");
  checkEq(machine.mutationRevision(kM1), u64{1}, "and the revision it publishes");
  checkEq(machine.backingContent(kB2), u64{0},
          "but the bytes have not moved: that is the offload");

  check(machine.step({Action::CommitMutation, kM1}), "commit enabled");
  check(machine.taskCarriesRotatedTarget(), "R-BACK-44.2a lease is concrete");
  check(machine.step({Action::CommitChunk, kC1}),
        "a chunk commit is enabled again once the transaction closed");
  check(machine.safety(), "transaction-order trace satisfies safety");
}

// Trace D — R-BACK-44.2's retryable pre-effect rejection. Staging or admission
// failed; the reservation is released and nothing visible moved, so the unlock
// can be retried with every layer consistent.
void testTraceAbortedReservationIsPreEffect() {
  Machine machine;
  check(machine.step({Action::Reserve, kM1}), "reserve enabled");
  const u64 ordinalAfterReserve = machine.nextOrdinal();
  check(machine.step({Action::AbortReserve, kM1}), "abort enabled");

  checkEq(machine.recordRevision(), u64{0}, "no revision bump");
  checkEq(machine.recordBacking(), kB1, "no rotation");
  checkEq(machine.logicalRevision(), u64{0}, "nothing published");
  checkEq(machine.mutationOrdinal(kM1), mo::kUnreservedOrdinal,
          "the reservation was released");
  check(machine.publishedRevisionCount(), "published count still zero");
  check(machine.recordRevisionCountsRotations(), "rotation count still zero");
  check(machine.safety(), "an aborted reservation leaves a safe state");

  // The released position is burned, not reused: the FIFO cursor only moves
  // forward, which is what keeps a retry from landing ahead of work already
  // admitted.
  checkEq(machine.nextOrdinal(), ordinalAfterReserve, "cursor did not regress");
  check(!machine.step({Action::CommitMutation, kM1}),
        "an aborted task cannot be committed");

  // And the retry is a clean transaction with a fresh, later ordinal.
  check(machine.step({Action::Reserve, kM1}), "retry enabled");
  checkEq(machine.mutationOrdinal(kM1), ordinalAfterReserve,
          "the retry took the next position, not the burned one");
  check(machine.step({Action::Rotate, kM1, kB2}), "retry rotates");
  check(machine.step({Action::CommitMutation, kM1}), "retry commits");
  check(machine.safety(), "the retried transaction satisfies safety");
}

// Trace E — two mutations and three chunks: the domain the production
// configuration checks, exercised as one concrete interleaving. It covers the
// A -> B -> A backing reuse, which is admissible only because the chunk that
// captured the first entry has finished encoding by then.
void testTraceBackingReuseAfterEncode() {
  const std::array<Step, 12> steps{{
      {Action::CommitChunk, kC1},      // ordinal 1, captures (b1, rev0)
      {Action::Reserve, kM1},          // ordinal 2
      {Action::Rotate, kM1, kB2},
      {Action::CommitMutation, kM1},
      {Action::ReplayChunk, kC1},
      {Action::ApplyMutation, kM1},    // b2 := rev1
      {Action::EncodeChunk, kC1},      // c1 is done with b1
      {Action::Reserve, kM2},          // ordinal 3
      {Action::Rotate, kM2, kB1},      // reuse b1 — legal now, not before
      {Action::CommitMutation, kM2},
      {Action::CommitChunk, kC2},      // ordinal 4, captures (b1, rev2)
      {Action::ApplyMutation, kM2},    // b1 := rev2
  }};
  Machine machine = replay(steps, "backing-reuse-after-encode");

  check(machine.safety(), "reuse after encode must satisfy safety");
  checkEq(machine.backingContent(kB1), u64{2}, "b1 now holds revision 2");
  check(machine.step({Action::ReplayChunk, kC2}), "replay c2");
  check(machine.step({Action::EncodeChunk, kC2}), "encode c2");
  check(machine.encodeReadsAppliedBytes(), "c2 reads revision 2 from b1");
  check(machine.noLiveCaptureOverwritten(), "no live capture was retargeted");
}

// Trace F — the TLC counterexample for premise 1, translated verbatim from
// `BufferMutationOffload.counterexample.cfg`:
//
//   State 2: Reserve(m1)          ordinal 1
//   State 3: Rotate(m1, b1)       recordRevision -> 1
//   State 4: CommitMutation(m1)   unlock returns
//   State 5: CommitChunk(c1)      ordinal 2, captures (b1, rev1)
//   State 6: ReplayChunk(c1)      <- only reachable with the FIFO premise gone
//   State 7: EncodeChunk(c1)      <- Invariant EncodeReadsAppliedBytes violated
//
// This is the rejected second-queue design: the chunk is replayed and encoded
// while the mutation task ahead of it in producer order has not been applied,
// so the draw renders the pre-unlock bytes out of a backing whose revision
// says otherwise. Note that TLC's rotation stays on `b1` — legal, because no
// live chunk had captured it — which makes the point sharper: the fault is
// purely an ordering one, not a backing-selection one.
void testTraceCounterexampleSecondQueue() {
  const std::array<Step, 6> steps{{
      {Action::Reserve, kM1},
      {Action::Rotate, kM1, kB1},
      {Action::CommitMutation, kM1},
      {Action::CommitChunk, kC1},
      {Action::ReplayChunk, kC1},
      {Action::EncodeChunk, kC1},
  }};
  const Machine machine =
      replay(steps, "tlc-fifo-counterexample", FifoDiscipline::Removed);

  checkEq(machine.capturedRevision(kC1), u64{1}, "c1 captured the new revision");
  checkEq(machine.backingContent(kB1), u64{0}, "the bytes never landed");
  check(!machine.encodeReadsAppliedBytes(),
        "the second-queue trace must reproduce the stale encode read");
  check(!machine.fifoApplicationOrder(),
        "and it violates the ordering premise it deleted");
  // Everything else is untouched: the deletion is surgical.
  check(machine.snapshotRevisionIsCurrent(), "rotation stayed synchronous");
  check(machine.noLiveCaptureOverwritten(), "the ring gate still held");
  check(machine.taskCarriesRotatedTarget(), "the task still owned its lease");

  // Under the production discipline the trace dies at its fifth step, which is
  // exactly what the single queue buys.
  Machine production;
  check(production.step({Action::Reserve, kM1}), "reserve");
  check(production.step({Action::Rotate, kM1, kB1}), "rotate");
  check(production.step({Action::CommitMutation, kM1}), "commit task");
  check(production.step({Action::CommitChunk, kC1}), "commit chunk");
  check(!production.step({Action::ReplayChunk, kC1}),
        "replay must be disabled until the mutation ahead of it is applied");
  check(production.step({Action::ApplyMutation, kM1}), "apply first");
  check(production.step({Action::ReplayChunk, kC1}), "then replay");
  check(production.step({Action::EncodeChunk, kC1}), "then encode");
  check(production.safety(), "the ordered trace is sound");
}

// Trace G — the TLC counterexample for premise 2, translated verbatim from
// `BufferMutationOffload.rotation.counterexample.cfg`:
//
//   State 2: Reserve(m1)          ordinal 1
//   State 3: CommitMutation(m1)   unlock returns; published = 1, record = 0
//   State 4: CommitChunk(c1)      <- Invariant SnapshotRevisionIsCurrent
//                                    violated: captures (b1, rev0)
//
// The logical rotation was deferred to the worker, so the capture freezes the
// pre-unlock backing and revision for a draw the app has already published new
// content for. The read that follows is perfectly coherent — the old backing
// really does still hold the old revision — which is why this premise needs
// its own invariant and its own configuration.
void testTraceCounterexampleDeferredRotation() {
  const std::array<Step, 3> steps{{
      {Action::Reserve, kM1},
      {Action::CommitMutation, kM1},
      {Action::CommitChunk, kC1},
  }};
  Machine machine = replay(steps, "tlc-rotation-counterexample",
                           FifoDiscipline::Enforced, RotationDiscipline::Deferred);

  checkEq(machine.logicalRevision(), u64{1}, "the app published revision 1");
  checkEq(machine.recordRevision(), u64{0}, "the record still says revision 0");
  checkEq(machine.capturedRevision(kC1), u64{0}, "so the capture is stale");
  checkEq(machine.capturedBacking(kC1), kB1, "and names the pre-rotation entry");
  check(!machine.snapshotRevisionIsCurrent(),
        "the deferred-rotation trace must reproduce the stale snapshot");
  check(!captureRevisionIsCurrent(machine.capturedRevision(kC1),
                                  machine.logicalRevision()),
        "the shared predicate reports it, not a restatement of it");

  // The damage is invisible to the visibility invariant: continue the trace
  // and the byte read still agrees with its own snapshot.
  check(machine.step({Action::ApplyMutation, kM1, kB2}), "worker rotates late");
  check(machine.step({Action::ReplayChunk, kC1}), "replay");
  check(machine.step({Action::EncodeChunk, kC1}), "encode");
  check(machine.encodeReadsAppliedBytes(),
        "a stale snapshot is read coherently — which is why it needs its own "
        "invariant");

  // Under the production discipline the trace dies at its second step: the
  // commit is not reachable until the rotation has run.
  Machine production;
  check(production.step({Action::Reserve, kM1}), "reserve");
  check(!production.step({Action::CommitMutation, kM1}),
        "commit must be disabled until the synchronous rotation has run");
  check(production.step({Action::Rotate, kM1, kB2}), "rotate");
  check(production.step({Action::CommitMutation, kM1}), "then commit");
  check(production.step({Action::CommitChunk, kC1}), "commit chunk");
  checkEq(production.capturedRevision(kC1), u64{1}, "the capture is current");
  check(production.snapshotRevisionIsCurrent(), "no stale snapshot");
  check(production.safety(), "the synchronous trace is sound");
}

}  // namespace

int main() {
  try {
    testPlainWritableLockTruthTable();
    testAdmissionTruthTable();
    testFifoOrdinalPrecedesTruthTable();
    testFifoBlockingTruthTable();
    testCaptureRevisionTruthTable();
    testDirectReaderFenceTruthTable();
    testTraceProductionOrdering();
    testTraceEncodeDecoupledFromMutation();
    testTraceTransactionOrder();
    testTraceAbortedReservationIsPreEffect();
    testTraceBackingReuseAfterEncode();
    testTraceCounterexampleSecondQueue();
    testTraceCounterexampleDeferredRotation();
  } catch (const TestFailure& failure) {
    std::cerr << "buffer_mutation_offload_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "buffer_mutation_offload_spec unexpected exception: "
              << ex.what() << '\n';
    return 1;
  }

  std::cout << "buffer_mutation_offload_spec passed\n";
  return 0;
}
