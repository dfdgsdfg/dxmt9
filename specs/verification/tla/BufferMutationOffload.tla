---- MODULE BufferMutationOffload ----
(*
 * dxmt9 Managed Buffer Mutation Offload — TLA+ Specification
 *
 * Models R-BACK-44.2 / 44.2a / 44.3 / 44.4 (`specs/backend/buffer-mutation-
 * offload/{requirements,spec}.md`): deferring the BYTE MATERIALIZATION of a
 * plain Managed-pool writable unlock from the producer thread onto the
 * commit-replay offload worker, while every draw still observes exactly the
 * content it observes today.
 *
 * The split under test:
 *
 *   producer (synchronous)  reserve a FIFO ordinal -> rotate the rename ring
 *                           (backing + contentRevision) -> commit the task
 *   worker   (deferred)     apply the staged bytes to the leased backing, in
 *                           ONE strict FIFO order shared with chunk replay
 *   encode   (decoupled)    reads a chunk's captured (backing, revision) any
 *                           time after that chunk's replay — NOT in FIFO order
 *
 * TWO independent premises are checked, and each has a purpose-built
 * counterexample configuration:
 *
 *  1. FIFO APPLICATION (`FifoDiscipline`). Design §4: "No second queue — one
 *     queue is what makes the ordering argument one sentence." A mutation task
 *     enqueued between chunk A and chunk B is applied after A's replay and
 *     before B's replay, so by the time B is replayed — and therefore by the
 *     time B is encoded — the bytes are in the backing B captured.
 *     `FifoDiscipline = "Removed"` models exactly the rejected alternative: a
 *     SECOND queue, on which mutations stay FIFO among themselves but are
 *     unordered against chunk replay. A later chunk is then replayed and
 *     encoded while its captured backing still holds pre-mutation bytes.
 *
 *  2. SYNCHRONOUS ROTATION (`RotationDiscipline`). R-BACK-44.2 step 2: the
 *     LOGICAL rotation (`renameActiveIndex` / `record.buffer` /
 *     `record.contents` / `contentRevision`) runs on the producer, between
 *     reserve and commit — only the bytes move to the worker.
 *     `RotationDiscipline = "Deferred"` moves the rotation itself onto the
 *     worker. `Pool::captureChunkBufferBinding` then freezes the PRE-rotation
 *     backing and revision for a chunk committed after the unlock returned,
 *     i.e. a draw the app has already published new content for captures the
 *     old content. The bytes it later reads are self-consistent — which is why
 *     this failure is NOT visible through premise 1's invariant and needs its
 *     own.
 *
 * Actors:
 *
 *   Producer  — Reserve -> Rotate -> CommitMutation is the R-BACK-44.2
 *               transaction, in that order; AbortReserve is its pre-effect
 *               rejection path (step 1 failed: no rotation, no revision bump).
 *               CommitChunk is `commit_chunk`'s synchronous capture. All four
 *               run on the one app thread, so `ProducerBusy` excludes a chunk
 *               commit from landing inside an unlock transaction.
 *   Worker    — ReplayChunk / ApplyMutation, the single `ReplayOffloadQueue`
 *               drain loop dispatching per task alternative.
 *   Encode    — EncodeChunk, the encode worker's byte read of a chunk's
 *               captured snapshot. Deliberately carries NO cross-chunk order:
 *               R-BACK-44.4a's whole point is that encode of an earlier chunk
 *               may run after a later mutation has been applied.
 *
 * Content is modelled as revision identity, not bytes: `backingContent[b]` is
 * the `contentRevision` whose bytes backing `b` currently holds. A chunk's
 * encode-side read is sound exactly when the backing it captured holds the
 * revision it captured. That abstraction is what lets one small state space
 * decide the visibility question; it does not model byte layout, the
 * copy-forward/dirty-patch split, or the pool shadow separately.
 *
 * Scope / non-claims:
 *   - Backing SELECTION safety is `BufferBackingVersioning.tla` (R-BACK-5.11)
 *     and is not re-proved here. This model imports its conclusion as the
 *     `BackingFreeForRotation` gate, in the deliberately WEAKER form "not
 *     captured by a chunk that has not finished encoding". Production's gate
 *     (`backingLastUsed[b] <= completedSeqId`) is strictly stronger, since GPU
 *     completion is downstream of encode, so every behaviour production admits
 *     this model admits too. Neither `NoUploadOverwriteInFlight` nor
 *     `NoBackingFreedInFlight` is weakened: no action here frees a backing or
 *     writes one the gate excluded.
 *   - It does not model the staged-byte budget, the queue's chunk/byte bounds,
 *     the R-BACK-44.2a residency lease against destroy/GC, worker fail-stop
 *     poisoning (R-BACK-44.7), or the R-BACK-44.5 direct-reader fence — that
 *     last one is `ReplayScopedDrain.tla`'s `ScopedReturnSafe`.
 *   - It does not model Metal, pixels, or driver behaviour.
 *
 * Properties verified:
 *   Safety   — TypeOK, EncodeReadsAppliedBytes, SnapshotRevisionIsCurrent,
 *              NoLiveCaptureOverwritten, FifoApplicationOrder,
 *              OrdinalsAreUnique, PublishedRevisionCount,
 *              RecordRevisionCountsRotations, TaskCarriesRotatedTarget
 *   Action   — RecordRevisionMonotonic, LogicalRevisionMonotonic,
 *              OrdinalMonotonic
 *
 * Counterexample configurations (both executed by
 * `scripts/check/verify_tla.sh` as expected failures):
 *   .counterexample           FifoDiscipline="Removed"
 *                             -> EncodeReadsAppliedBytes
 *   .rotation.counterexample  RotationDiscipline="Deferred"
 *                             -> SnapshotRevisionIsCurrent
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Backings,           \* set of concrete rename-ring entries (e.g. {b1,b2,b3})
  NoBacking,          \* sentinel: no backing captured / no target leased yet
  NumChunks,          \* model bound on committed chunks
  NumMutations,       \* model bound on Managed writable unlocks
  MAX_ORDINAL,        \* model bound on the shared FIFO ordinal domain
  FifoDiscipline,     \* "Enforced" (production) | "Removed" (counterexample)
  RotationDiscipline  \* "Synchronous" (production) | "Deferred" (counterexample)

ASSUME Backings # {}
ASSUME NoBacking \notin Backings
ASSUME NumChunks \in Nat /\ NumChunks >= 1
ASSUME NumMutations \in Nat /\ NumMutations >= 1
ASSUME MAX_ORDINAL \in Nat /\ MAX_ORDINAL >= NumChunks + NumMutations
ASSUME FifoDiscipline \in {"Enforced", "Removed"}
ASSUME RotationDiscipline \in {"Synchronous", "Deferred"}

Chunks    == 1..NumChunks
Mutations == 1..NumMutations
Revisions == 0..NumMutations
Ordinals  == 0..MAX_ORDINAL          \* 0 means "holds no FIFO reservation"

ChunkPhases    == {"Idle", "Committed", "Replayed", "Encoded"}
MutationPhases == {"Idle", "Reserved", "Rotated", "Committed", "Applied"}

InitialBacking == CHOOSE b \in Backings : TRUE

VARIABLES
  recordBacking,     \* Backings — BufferRecord::buffer / renameActiveIndex
  recordRevision,    \* Revisions — BufferRecord::contentRevision
  logicalRevision,   \* Revisions — revisions the APP has published (unlocked)
  backingContent,    \* Backings -> Revisions — bytes each backing holds
  nextOrdinal,       \* Nat — next free position in the shared FIFO
  chunkPhase,        \* Chunks -> ChunkPhases
  chunkOrdinal,      \* Chunks -> Ordinals
  capturedBacking,   \* Chunks -> Backings \cup {NoBacking}
  capturedRevision,  \* Chunks -> Revisions
  mutationPhase,     \* Mutations -> MutationPhases
  mutationOrdinal,   \* Mutations -> Ordinals
  mutationTarget,    \* Mutations -> Backings \cup {NoBacking} — leased entry
  mutationRevision,  \* Mutations -> Revisions — revision this task publishes
  staleEncodeRead,   \* BOOLEAN — sticky fault flag (see EncodeReadsAppliedBytes)
  staleSnapshot      \* BOOLEAN — sticky fault flag (see SnapshotRevisionIsCurrent)

vars ==
  <<recordBacking, recordRevision, logicalRevision, backingContent, nextOrdinal,
    chunkPhase, chunkOrdinal, capturedBacking, capturedRevision, mutationPhase,
    mutationOrdinal, mutationTarget, mutationRevision, staleEncodeRead,
    staleSnapshot>>

(* ================================================================
   Shared predicate vocabulary
   ----------------------------------------------------------------
   These three operators are the model half of the shared pure predicates in
   `src/dxmt9/dxmt9_mutation_offload_predicates.hpp`. The future production
   code and `tests/native/backend/buffer_mutation_offload_spec.cpp` call the
   C++ twins by the same names, so a TLC trace is mechanically translatable
   into a native step sequence — the binding pattern
   `dxmt9_mark_reclaim_predicates.hpp` / `dxmt9-producer-mark-reclaim-spec`
   established for R-BACK-43.6.
   ================================================================ *)

\* C++: dxmt9::resources::mutation_offload::fifoOrdinalPrecedes
\* An unreserved item (ordinal 0) never precedes anything: it holds no
\* position in the queue at all, so it can impose no order.
FifoOrdinalPrecedes(earlier, later) ==
  /\ earlier # 0
  /\ later # 0
  /\ earlier < later

\* C++: dxmt9::resources::mutation_offload::mutationBlocksChunkReplay
\* R-BACK-44.3, the ReplayChunk half: a committed mutation ahead of this chunk
\* in the one FIFO must be applied before the chunk is replayed.
MutationBlocksChunkReplay(mOrdinal, mApplied, cOrdinal) ==
  /\ FifoOrdinalPrecedes(mOrdinal, cOrdinal)
  /\ ~mApplied

\* C++: dxmt9::resources::mutation_offload::chunkBlocksMutationApply
\* R-BACK-44.3, the ApplyMutation half: a committed chunk ahead of this
\* mutation must be replayed before the mutation is applied.
ChunkBlocksMutationApply(cOrdinal, cReplayed, mOrdinal) ==
  /\ FifoOrdinalPrecedes(cOrdinal, mOrdinal)
  /\ ~cReplayed

\* C++: dxmt9::resources::mutation_offload::captureRevisionIsCurrent
\* R-BACK-44.4: the snapshot `Pool::captureChunkBufferBinding` freezes carries
\* the revision the app has actually published, because R-BACK-44.2 rotated
\* synchronously before the unlock returned.
CaptureRevisionIsCurrent(captured, published) == captured = published

(* ================================================================
   Derived state
   ================================================================ *)

ChunkCommitted(c) == chunkPhase[c] # "Idle"
ChunkReplayed(c)  == chunkPhase[c] \in {"Replayed", "Encoded"}

\* A chunk between its commit and its encode. Its captured backing is a live
\* read target: the encode has not consumed it yet.
ChunkLive(c) == chunkPhase[c] \in {"Committed", "Replayed"}

MutationApplied(m) == mutationPhase[m] = "Applied"

\* The one app thread is inside an unlock transaction. `commit_chunk` cannot
\* interleave with it, which is what makes reserve/rotate/commit atomic against
\* capture without any lock appearing in this model.
ProducerBusy == \E m \in Mutations : mutationPhase[m] \in {"Reserved", "Rotated"}

(*
 * BackingFreeForRotation(b)
 * The R-BACK-5.11 backing-selection gate, imported from
 * `BufferBackingVersioning.tla` in its weaker encode-scoped form (see the
 * header's scope note). A backing captured by a chunk that has not finished
 * encoding is still going to be read through that capture, so a rotation may
 * not retarget it. Production reaches the same exclusion through
 * `backingLastUsed[b] <= completedSeqId`, and GPU completion is downstream of
 * encode.
 *)
BackingFreeForRotation(b) ==
  \A c \in Chunks : ChunkLive(c) => capturedBacking[c] # b

\* Whichever actor performed the logical rotation for m has done so.
RotationDone(m) ==
  IF RotationDiscipline = "Synchronous"
  THEN mutationPhase[m] \in {"Rotated", "Committed", "Applied"}
  ELSE mutationPhase[m] = "Applied"

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ recordBacking    = InitialBacking
  /\ recordRevision   = 0
  /\ logicalRevision  = 0
  /\ backingContent   = [b \in Backings |-> 0]
  /\ nextOrdinal      = 1
  /\ chunkPhase       = [c \in Chunks |-> "Idle"]
  /\ chunkOrdinal     = [c \in Chunks |-> 0]
  /\ capturedBacking  = [c \in Chunks |-> NoBacking]
  /\ capturedRevision = [c \in Chunks |-> 0]
  /\ mutationPhase    = [m \in Mutations |-> "Idle"]
  /\ mutationOrdinal  = [m \in Mutations |-> 0]
  /\ mutationTarget   = [m \in Mutations |-> NoBacking]
  /\ mutationRevision = [m \in Mutations |-> 0]
  /\ staleEncodeRead  = FALSE
  /\ staleSnapshot    = FALSE

(* ================================================================
   Producer — the R-BACK-44.2 reserve / rotate / commit transaction
   ================================================================ *)

(*
 * Reserve(m)  — R-BACK-44.2 step 1.
 * Fix the FIFO ordinal and charge the staged-byte budget with NO externally
 * visible side effect, then stage the dirty span into task-owned storage.
 * Everything fallible happens here, which is why the ordinal is taken FIRST:
 * rotating before enqueuing would let a concurrent producer chunk overtake the
 * mutation, and a rejected push would leave a visible rotation with no task.
 *
 * The `m2 < m` conjunct is a symmetry reduction only — mutation identifiers
 * are labels, so exploring their permutations adds states and no behaviours.
 *)
Reserve(m) ==
  /\ mutationPhase[m] = "Idle"
  /\ ~ProducerBusy
  /\ \A m2 \in Mutations : m2 < m => mutationPhase[m2] # "Idle"
  /\ nextOrdinal <= MAX_ORDINAL
  /\ mutationOrdinal' = [mutationOrdinal EXCEPT ![m] = nextOrdinal]
  /\ nextOrdinal'     = nextOrdinal + 1
  /\ mutationPhase'   = [mutationPhase EXCEPT ![m] = "Reserved"]
  /\ UNCHANGED <<recordBacking, recordRevision, logicalRevision, backingContent,
                 chunkPhase, chunkOrdinal, capturedBacking, capturedRevision,
                 mutationTarget, mutationRevision, staleEncodeRead,
                 staleSnapshot>>

(*
 * AbortReserve(m)  — R-BACK-44.2's retryable pre-effect rejection.
 * Staging or reservation failed, or the queue was stopped/poisoned. The
 * reservation is released and NOTHING else moved: no rotation, no revision
 * bump, no enqueue — and, in production, no lock-state clearing on any layer,
 * so the unlock is retryable with all layers consistent (review finding 8).
 * `PublishedRevisionCount` and `RecordRevisionCountsRotations` are what state
 * "nothing else moved" as checkable facts.
 *
 * The burned ordinal is deliberate: `release` returns the byte budget, not the
 * position, and a hole in the FIFO is inert because every ordering guard
 * quantifies over items that hold a reservation.
 *)
AbortReserve(m) ==
  /\ mutationPhase[m] = "Reserved"
  /\ mutationPhase'   = [mutationPhase EXCEPT ![m] = "Idle"]
  /\ mutationOrdinal' = [mutationOrdinal EXCEPT ![m] = 0]
  /\ UNCHANGED <<recordBacking, recordRevision, logicalRevision, backingContent,
                 nextOrdinal, chunkPhase, chunkOrdinal, capturedBacking,
                 capturedRevision, mutationTarget, mutationRevision,
                 staleEncodeRead, staleSnapshot>>

(*
 * Rotate(m, b)  — R-BACK-44.2 step 2, THE PREMISE.
 * The logical rename-ring rotation, synchronously on the producer under the
 * buffer arena's unique lock: select a backing, publish it as the record's
 * active entry, bump `contentRevision`. The task leases that concrete entry
 * (R-BACK-44.2a) — `mutationTarget[m]` — and the worker applies to the LEASE,
 * never to the record's then-current active backing, which is why
 * ApplyMutation below reads `mutationTarget[m]` rather than `recordBacking`.
 *
 * Note what this action does NOT do: it does not touch `backingContent`. That
 * is precisely the byte motion being offloaded, and the whole model exists to
 * decide whether every reader still sees it in time.
 *
 * `RotationDiscipline = "Deferred"` deletes this action and moves its body
 * into ApplyMutation, and nothing else.
 *)
Rotate(m, b) ==
  /\ RotationDiscipline = "Synchronous"
  /\ mutationPhase[m] = "Reserved"
  /\ BackingFreeForRotation(b)
  /\ recordRevision < NumMutations
  /\ recordBacking'    = b
  /\ recordRevision'   = recordRevision + 1
  /\ mutationTarget'   = [mutationTarget EXCEPT ![m] = b]
  /\ mutationRevision' = [mutationRevision EXCEPT ![m] = recordRevision + 1]
  /\ mutationPhase'    = [mutationPhase EXCEPT ![m] = "Rotated"]
  /\ UNCHANGED <<logicalRevision, backingContent, nextOrdinal, chunkPhase,
                 chunkOrdinal, capturedBacking, capturedRevision,
                 mutationOrdinal, staleEncodeRead, staleSnapshot>>

(*
 * CommitMutation(m)  — R-BACK-44.2 step 3, infallible.
 * Publish the task at its reserved FIFO position. `logicalRevision` is the
 * app's view: the number of unlocks that have returned, i.e. the content the
 * app believes every subsequent draw will see. Under the production discipline
 * `recordRevision` already equals it here, because step 2 ran first; under the
 * deferred discipline it does not, and CommitChunk records the divergence.
 *)
CommitMutation(m) ==
  /\ mutationPhase[m] =
       (IF RotationDiscipline = "Synchronous" THEN "Rotated" ELSE "Reserved")
  /\ logicalRevision < NumMutations
  /\ mutationPhase'  = [mutationPhase EXCEPT ![m] = "Committed"]
  /\ logicalRevision' = logicalRevision + 1
  /\ UNCHANGED <<recordBacking, recordRevision, backingContent, nextOrdinal,
                 chunkPhase, chunkOrdinal, capturedBacking, capturedRevision,
                 mutationOrdinal, mutationTarget, mutationRevision,
                 staleEncodeRead, staleSnapshot>>

(*
 * CommitChunk(c)  — R-BACK-44.4's synchronous capture.
 * `Pool::captureChunkBufferBinding` freezes the LIVE record at commit time:
 * backing handle, contents address, and `contentRevision`. The draw encoder
 * reads only that snapshot for versioned records, so this value — not the
 * record — is what the chunk's draws will observe.
 *
 * `~ProducerBusy` is the single-thread fact, not a lock: the app cannot call
 * a draw/commit path from inside its own `Unlock`.
 *)
CommitChunk(c) ==
  /\ chunkPhase[c] = "Idle"
  /\ \A c2 \in Chunks : c2 < c => chunkPhase[c2] # "Idle"
  /\ ~ProducerBusy
  /\ nextOrdinal <= MAX_ORDINAL
  /\ chunkOrdinal'     = [chunkOrdinal EXCEPT ![c] = nextOrdinal]
  /\ nextOrdinal'      = nextOrdinal + 1
  /\ capturedBacking'  = [capturedBacking EXCEPT ![c] = recordBacking]
  /\ capturedRevision' = [capturedRevision EXCEPT ![c] = recordRevision]
  /\ chunkPhase'       = [chunkPhase EXCEPT ![c] = "Committed"]
  \* The parentheses are load-bearing: TLA+ binds `=` tighter than `\/`, so the
  \* unparenthesized form would parse as `(staleSnapshot' = staleSnapshot) \/
  \* (...)` and leave the fault flag unconstrained.
  /\ staleSnapshot' =
       (staleSnapshot \/ ~CaptureRevisionIsCurrent(recordRevision, logicalRevision))
  /\ UNCHANGED <<recordBacking, recordRevision, logicalRevision, backingContent,
                 mutationPhase, mutationOrdinal, mutationTarget,
                 mutationRevision, staleEncodeRead>>

(* ================================================================
   Worker — one FIFO, two task alternatives (R-BACK-44.3)
   ================================================================ *)

(*
 * ReplayChunk(c)
 * The offload worker replays a raw chunk. Two ordering premises:
 *   - chunks are FIFO among themselves (true in every configuration; that is
 *     the queue that already exists today); and
 *   - under `FifoDiscipline = "Enforced"`, an earlier mutation task must
 *     already be applied, because it occupies a position in the SAME queue.
 * `FifoDiscipline = "Removed"` deletes only the second conjunct — the rejected
 * "second queue" design in which mutations are ordered among themselves but
 * not against chunk replay.
 *)
ReplayChunk(c) ==
  /\ chunkPhase[c] = "Committed"
  /\ \A c2 \in Chunks :
       FifoOrdinalPrecedes(chunkOrdinal[c2], chunkOrdinal[c]) => ChunkReplayed(c2)
  /\ (FifoDiscipline = "Enforced" =>
        \A m \in Mutations :
          ~MutationBlocksChunkReplay(mutationOrdinal[m], MutationApplied(m),
                                     chunkOrdinal[c]))
  /\ chunkPhase' = [chunkPhase EXCEPT ![c] = "Replayed"]
  /\ UNCHANGED <<recordBacking, recordRevision, logicalRevision, backingContent,
                 nextOrdinal, chunkOrdinal, capturedBacking, capturedRevision,
                 mutationPhase, mutationOrdinal, mutationTarget,
                 mutationRevision, staleEncodeRead, staleSnapshot>>

(*
 * ApplyMutation(m)  — R-BACK-44.3.
 * Copy-forward from the pool shadow, dirty-span patch, shadow update — all of
 * it collapsed to "the leased backing now holds this task's revision", because
 * the induction R-BACK-44.3 relies on ("the shadow holds exactly the
 * pre-mutation content by this same order") is the ordering fact this model
 * checks, not a byte fact.
 *
 * Mutations stay FIFO among themselves in EVERY configuration: even the
 * rejected second-queue design would not reorder its own queue. Only the
 * chunk-vs-mutation conjunct is discipline-gated, so `FifoDiscipline` deletes
 * exactly one premise and no more.
 *
 * Under `RotationDiscipline = "Deferred"` the worker also performs the logical
 * rotation here — the same body Rotate has under the production discipline,
 * moved to the wrong side of the unlock.
 *)
ApplyMutation(m) ==
  /\ mutationPhase[m] = "Committed"
  /\ \A m2 \in Mutations :
       FifoOrdinalPrecedes(mutationOrdinal[m2], mutationOrdinal[m])
         => MutationApplied(m2)
  /\ (FifoDiscipline = "Enforced" =>
        \A c \in Chunks :
          ~ChunkBlocksMutationApply(chunkOrdinal[c], ChunkReplayed(c),
                                    mutationOrdinal[m]))
  /\ IF RotationDiscipline = "Synchronous"
     THEN /\ backingContent' =
               [backingContent EXCEPT ![mutationTarget[m]] = mutationRevision[m]]
          /\ UNCHANGED <<recordBacking, recordRevision, mutationTarget,
                         mutationRevision>>
     ELSE \E b \in Backings :
            /\ BackingFreeForRotation(b)
            /\ recordRevision < NumMutations
            /\ recordBacking'    = b
            /\ recordRevision'   = recordRevision + 1
            /\ mutationTarget'   = [mutationTarget EXCEPT ![m] = b]
            /\ mutationRevision' = [mutationRevision EXCEPT ![m] = recordRevision + 1]
            /\ backingContent'   = [backingContent EXCEPT ![b] = recordRevision + 1]
  /\ mutationPhase' = [mutationPhase EXCEPT ![m] = "Applied"]
  /\ UNCHANGED <<logicalRevision, nextOrdinal, chunkPhase, chunkOrdinal,
                 capturedBacking, capturedRevision, mutationOrdinal,
                 staleEncodeRead, staleSnapshot>>

(* ================================================================
   Encode — the decoupled reader (R-BACK-44.4 / 44.4a)
   ================================================================ *)

(*
 * EncodeChunk(c)
 * The encode worker consumes a replayed chunk and reads buffer bytes through
 * the captured snapshot. Its ONLY ordering premise is its own chunk's replay:
 * there is deliberately no cross-chunk order here, because R-BACK-44.4a is
 * exactly the observation that encode of an EARLIER chunk may run after a
 * LATER mutation has been applied. A model that ordered encode would hide the
 * class of bug 44.4a exists to exclude.
 *
 * `backingContent[capturedBacking[c]] # capturedRevision[c]` is the fault: the
 * chunk's draws are reading a backing that does not (yet, or any more) hold
 * the revision the chunk captured. It is recorded rather than excluded by
 * fiat, so the counterexample configuration can reach it — production code
 * re-checks nothing here either.
 *)
EncodeChunk(c) ==
  /\ chunkPhase[c] = "Replayed"
  /\ chunkPhase' = [chunkPhase EXCEPT ![c] = "Encoded"]
  /\ staleEncodeRead' =
       (staleEncodeRead \/ backingContent[capturedBacking[c]] # capturedRevision[c])
  /\ UNCHANGED <<recordBacking, recordRevision, logicalRevision, backingContent,
                 nextOrdinal, chunkOrdinal, capturedBacking, capturedRevision,
                 mutationPhase, mutationOrdinal, mutationTarget,
                 mutationRevision, staleSnapshot>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ \E m \in Mutations : Reserve(m)
  \/ \E m \in Mutations : AbortReserve(m)
  \/ \E m \in Mutations, b \in Backings : Rotate(m, b)
  \/ \E m \in Mutations : CommitMutation(m)
  \/ \E c \in Chunks : CommitChunk(c)
  \/ \E c \in Chunks : ReplayChunk(c)
  \/ \E m \in Mutations : ApplyMutation(m)
  \/ \E c \in Chunks : EncodeChunk(c)

Spec == Init /\ [][Next]_vars

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ recordBacking    \in Backings
  /\ recordRevision   \in Revisions
  /\ logicalRevision  \in Revisions
  /\ backingContent   \in [Backings -> Revisions]
  /\ nextOrdinal      \in 1..(MAX_ORDINAL + 1)
  /\ chunkPhase       \in [Chunks -> ChunkPhases]
  /\ chunkOrdinal     \in [Chunks -> Ordinals]
  /\ capturedBacking  \in [Chunks -> (Backings \cup {NoBacking})]
  /\ capturedRevision \in [Chunks -> Revisions]
  /\ mutationPhase    \in [Mutations -> MutationPhases]
  /\ mutationOrdinal  \in [Mutations -> Ordinals]
  /\ mutationTarget   \in [Mutations -> (Backings \cup {NoBacking})]
  /\ mutationRevision \in [Mutations -> Revisions]
  /\ staleEncodeRead  \in BOOLEAN
  /\ staleSnapshot    \in BOOLEAN
  \* An item holds a FIFO reservation exactly while it is past Idle. Chunks
  \* never leave Idle again; a mutation does, but only through AbortReserve,
  \* which releases the reservation in the same step.
  /\ \A c \in Chunks : ChunkCommitted(c) <=> chunkOrdinal[c] # 0
  /\ \A m \in Mutations : mutationPhase[m] # "Idle" <=> mutationOrdinal[m] # 0
  \* A committed chunk always carries a concrete captured backing, which is
  \* what makes EncodeChunk's `backingContent[capturedBacking[c]]` total.
  /\ \A c \in Chunks : ChunkCommitted(c) => capturedBacking[c] \in Backings

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * EncodeReadsAppliedBytes  — R-BACK-44.4, THE VISIBILITY CONTRACT.
 *
 * "Any consumer that reads buffer bytes on the replay/encode side for a chunk
 * enqueued after the mutation must observe the applied bytes; this is
 * discharged by R-BACK-44.3's ordering, not by any wait."
 *
 * Stated as: every encode-side byte read observes, in the backing that chunk
 * captured, exactly the `contentRevision` that chunk captured. Note the shape
 * this deliberately does NOT take — "the bytes that backing held at capture
 * time". Under offload the captured backing does not hold those bytes at
 * capture time; the rotation published a fresh entry and the task that fills
 * it is still queued. The contract is that it holds them by the time the
 * chunk is ENCODED, which is what the FIFO order buys, and stating it against
 * capture-time bytes would be unsatisfiable by construction.
 *
 * Two ways to break it, and the model can reach both:
 *   - the mutation has not been applied yet when the chunk that captured its
 *     target is encoded (premise 1, `.counterexample.cfg`); or
 *   - a LATER mutation retargeted a backing an earlier live chunk had already
 *     captured, so the read finds a revision from the future. That one is kept
 *     unreachable by `BackingFreeForRotation`, and `NoLiveCaptureOverwritten`
 *     states it separately.
 *)
EncodeReadsAppliedBytes == ~staleEncodeRead

(*
 * SnapshotRevisionIsCurrent  — R-BACK-44.2 step 2 / R-BACK-44.4.
 *
 * "every captured snapshot's revision equals the record revision at its commit
 * point" (design §6). `logicalRevision` is the record revision the app has
 * published — the count of unlocks that have returned — and `recordRevision`
 * is what a capture actually reads. Synchronous rotation makes them the same
 * value at every commit point; deferring the rotation to the worker does not,
 * and a chunk committed in the window between the unlock returning and the
 * task being applied then captures the OLD backing and OLD revision for draws
 * the app has already published new content for.
 *
 * This failure is invisible to `EncodeReadsAppliedBytes`: the stale snapshot
 * is internally consistent — the old backing really does still hold the old
 * revision — so the wrong content is read without any read being incoherent.
 * That is why the two premises need two invariants and two configurations.
 *)
SnapshotRevisionIsCurrent == ~staleSnapshot

(*
 * NoLiveCaptureOverwritten
 * The positional form of the second failure mode above, stated over the state
 * rather than through the fault flag, so a future change that loses the flag
 * wiring still fails here: an applied mutation never targets a backing that a
 * still-live chunk ahead of it in the FIFO had captured.
 *
 * It holds because `BackingFreeForRotation` excludes exactly those backings at
 * rotation time, and a chunk's liveness is monotone downward — an encoded
 * chunk never becomes live again, and a chunk committed after the rotation
 * necessarily carries a LATER ordinal (the mutation took its ordinal at
 * Reserve, and `~ProducerBusy` keeps a commit out of the transaction window).
 * That is `BufferBackingVersioning.tla`'s reuse rule doing the work; this model
 * imports it rather than re-proving it.
 *)
NoLiveCaptureOverwritten ==
  \A m \in Mutations :
    MutationApplied(m) =>
      \A c \in Chunks :
        (ChunkLive(c) /\ FifoOrdinalPrecedes(chunkOrdinal[c], mutationOrdinal[m]))
          => capturedBacking[c] # mutationTarget[m]

(*
 * FifoApplicationOrder  — R-BACK-44.3 stated directly.
 * "a mutation task enqueued after chunk A and before chunk B is applied after
 * A's replay completes and before B's replay begins". Both directions, over
 * the one shared ordinal domain. This is the premise, not the consequence:
 * `.counterexample.cfg` deletes it and is expected to be reported through
 * `EncodeReadsAppliedBytes` instead, so this invariant is deliberately not
 * listed there.
 *)
FifoApplicationOrder ==
  /\ \A c \in Chunks :
       ChunkReplayed(c) =>
         \A m \in Mutations :
           FifoOrdinalPrecedes(mutationOrdinal[m], chunkOrdinal[c])
             => MutationApplied(m)
  /\ \A m \in Mutations :
       MutationApplied(m) =>
         \A c \in Chunks :
           FifoOrdinalPrecedes(chunkOrdinal[c], mutationOrdinal[m])
             => ChunkReplayed(c)

(*
 * OrdinalsAreUnique
 * `reserve` fixes ONE position, and a chunk commit takes another: the two task
 * alternatives share a single ordinal domain, which is what "no second queue"
 * means as a checkable fact.
 *)
OrdinalsAreUnique ==
  /\ \A c1, c2 \in Chunks :
       (c1 # c2 /\ chunkOrdinal[c1] # 0) => chunkOrdinal[c1] # chunkOrdinal[c2]
  /\ \A m1, m2 \in Mutations :
       (m1 # m2 /\ mutationOrdinal[m1] # 0)
         => mutationOrdinal[m1] # mutationOrdinal[m2]
  /\ \A c \in Chunks, m \in Mutations :
       (chunkOrdinal[c] # 0 /\ mutationOrdinal[m] # 0)
         => chunkOrdinal[c] # mutationOrdinal[m]

(*
 * PublishedRevisionCount
 * The app-visible revision counts exactly the unlocks that COMMITTED. An
 * aborted reservation published nothing — this is the checkable half of
 * R-BACK-44.2's "retryable pre-effect rejection".
 *)
PublishedRevisionCount ==
  logicalRevision =
    Cardinality({m \in Mutations : mutationPhase[m] \in {"Committed", "Applied"}})

(*
 * RecordRevisionCountsRotations
 * And the record's own revision counts exactly the rotations that happened —
 * under either discipline, since `RotationDone` follows the rotation to
 * whichever actor performs it. Together with the invariant above this is the
 * conservation law that makes the two disciplines comparable at all: the same
 * rotations happen either way, only the point at which they become visible
 * moves.
 *)
RecordRevisionCountsRotations ==
  recordRevision = Cardinality({m \in Mutations : RotationDone(m)})

(*
 * TaskCarriesRotatedTarget  — R-BACK-44.2a, the task lease.
 * Under the production discipline every committed task already names a
 * concrete rename-ring entry and the revision it will publish, because
 * rotation ran strictly between reserve and commit. The worker therefore
 * applies to the LEASE and never has to consult the record's then-current
 * active backing. Vacuous under the deferred discipline, which is the point:
 * there the worker has no lease to apply to.
 *)
TaskCarriesRotatedTarget ==
  (RotationDiscipline = "Synchronous") =>
    \A m \in Mutations :
      mutationPhase[m] \in {"Committed", "Applied"} =>
        /\ mutationTarget[m] \in Backings
        /\ mutationRevision[m] # 0

Safety ==
  /\ TypeOK
  /\ EncodeReadsAppliedBytes
  /\ SnapshotRevisionIsCurrent
  /\ NoLiveCaptureOverwritten
  /\ FifoApplicationOrder
  /\ OrdinalsAreUnique
  /\ PublishedRevisionCount
  /\ RecordRevisionCountsRotations
  /\ TaskCarriesRotatedTarget

(* ================================================================
   Action properties
   ================================================================ *)

(* `contentRevision` is a counter, never a value that can regress. *)
RecordRevisionMonotonic == [][recordRevision' >= recordRevision]_vars

(* Neither is the app-published revision. *)
LogicalRevisionMonotonic == [][logicalRevision' >= logicalRevision]_vars

(* A released reservation returns the byte budget, never the position: the
   FIFO cursor only moves forward, so an abort cannot let a later item take an
   earlier ordinal than one already handed out. *)
OrdinalMonotonic == [][nextOrdinal' >= nextOrdinal]_vars

====
