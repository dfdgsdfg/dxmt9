---- MODULE CommandQueue ----
(*
 * dxmt9 CommandQueue — TLA+ Specification
 *
 * Models the correctness of the three-thread ring buffer that decouples the
 * Wine/application thread from Metal command encoding.
 *
 * Three concurrent agents:
 *
 *   WineThread   — fills CommandChunks with POD draw/blit command records;
 *                  commits on present() or when the chunk is full.
 *   EncodeThread — dequeues committed chunks in FIFO order; replays each
 *                  command record against ArgumentEncodingContext; submits the
 *                  resulting MTLCommandBuffer to the GPU. Per R-BACK-2.29,
 *                  encoding may produce a *chain* of MTLCommandBuffers on the
 *                  same MTLCommandQueue rather than a single buffer per chunk.
 *   FinishThread — waits for GPU completion signals; recycles chunk slots
 *                  back to Free; advances completedSeqId.
 *
 * Back-pressure: WineThread is blocked from committing a new chunk when
 * InflightCount >= MAX_INFLIGHT, preventing it from outpacing the GPU.
 *
 * Requirement traceability:
 *   R-BACK-2.1   Wine thread must not block on Metal API during draw submission
 *   R-BACK-2.2   Bounded queue; Wine thread outrun limit = MAX_INFLIGHT (default 3)
 *   R-BACK-2.3   Encode thread must not allocate on hot path (not modeled here)
 *   R-BACK-2.29  A chunk's encode produces a chain of 1..MAX_SUBCB MTLCommandBuffers
 *                on the same queue; the chunk's seqId covers the chain;
 *                completedSeqId advances only after the final sub-CB completes.
 *   R-BACK-2.30  Present metadata routes to the chain's last sub-CB only:
 *                presentTokenSignaled[seq] may rise only after the final
 *                sub-CB for that seq is GPU-completed.
 *   R-BACK-2.32  Reclaim keys off the chunk's completedSeqId; intermediate
 *                sub-CB completion never advances completedSeqId. Metal's
 *                same-queue in-order submission guarantees the chain's last
 *                sub-CB completes after every prior sub-CB; this model expresses
 *                that guarantee implicitly via per-slot sequential transitions
 *                (EncodeMidChunkCommit must fire before EncodeFinalCommit, which
 *                must fire before FinishComplete on that slot).
 *
 * Properties verified:
 *   Safety   — TypeOK, SeqIdSafety, BoundedInflight, RingSafety, EncodeSafety,
 *              SubCBProgressBounded, OnlyFinalAdvancesSeqId, PresentRoutedToTail
 *   Liveness — PendingEventuallyFree, EventuallyDrained
 *
 * How to run (TLC model checker):
 *   Use CommandQueue.cfg with RING_SIZE=4, MAX_INFLIGHT=2, MAX_SEQID=6,
 *   MAX_SUBCB=3. Full production values (32 slots, 3 inflight) make the state
 *   space too large for TLC; the small model exercises all structural behaviors
 *   including the multi-sub-CB chain (MAX_SUBCB >= 2 ensures coverage of both
 *   single-CB and chained-CB chunks).
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  RING_SIZE,    \* slots in the ring (use 4 for TLC; 32 in production)
  MAX_INFLIGHT, \* max simultaneously in-flight chunks (use 2 for TLC; 3 in production)
  MAX_SEQID,    \* model-checking upper bound on sequence IDs
  MAX_SUBCB     \* max sub-CBs per chain (R-BACK-2.29; >=1, with >=2 to exercise chaining)

ASSUME RING_SIZE \in Nat /\ RING_SIZE > 1
ASSUME MAX_INFLIGHT \in Nat /\ MAX_INFLIGHT >= 1
ASSUME MAX_INFLIGHT < RING_SIZE   \* ring must have slack beyond the inflight ceiling
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID > MAX_INFLIGHT
ASSUME MAX_SUBCB \in Nat /\ MAX_SUBCB >= 1

Slots == 0 .. (RING_SIZE - 1)
SubCBLengths == 1 .. MAX_SUBCB
SeqIds == 0 .. MAX_SEQID

(*
 * Slot lifecycle:
 *
 *   Free ──► Writing ──► Pending ──► Encoding ──► GPU ──► Free
 *             (Wine)      (Wine→     (Encode      (GPU→
 *                          Encode)    →GPU)        Finish)
 *
 *   While in Encoding, the slot may issue several mid-chunk sub-CB commits
 *   (R-BACK-2.29) before the final commit transitions the state to GPU.
 *)
ChunkStates == {"Free", "Writing", "Pending", "Encoding", "GPU"}

VARIABLES
  state,                \* FUNCTION Slots → ChunkStates
  chunkSeqId,           \* FUNCTION Slots → Nat  (seq ID assigned at commit; 0 otherwise)
  currentSeqId,         \* Nat — next seq ID to assign (starts at 1)
  completedSeqId,       \* Nat — seq ID of the most recently GPU-completed chunk (0 = none)
  writeIdx,             \* Slots — slot WineThread is currently filling / will fill next
  encodeIdx,            \* Slots — next slot EncodeThread will process
  subCBChainLength,     \* FUNCTION Slots → 0..MAX_SUBCB (0 when slot is Free; chain
                        \*   length chosen at WineCommit otherwise)
  subCBProgress,        \* FUNCTION Slots → Nat — sub-CBs already committed for this slot
  finalCommitted,       \* FUNCTION Slots → BOOLEAN — TRUE iff EncodeFinalCommit has fired
                        \*   for this slot in its current lifecycle
  slotIsPresent,        \* FUNCTION Slots → BOOLEAN — TRUE iff the chunk in this slot
                        \*   carries present metadata (R-BACK-2.30)
  presentTokenSignaled  \* FUNCTION SeqIds → BOOLEAN — present-frame token raised
                        \*   for this seqId (R-BACK-2.30); only ever set by
                        \*   FinishComplete after EncodeFinalCommit.

vars == <<state, chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx,
          subCBChainLength, subCBProgress, finalCommitted, slotIsPresent,
          presentTokenSignaled>>

InflightCount ==
  Cardinality({s \in Slots : state[s] \in {"Pending", "Encoding", "GPU"}})

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ state               = [s \in Slots  |-> "Free"]
  /\ chunkSeqId          = [s \in Slots  |-> 0]
  /\ currentSeqId        = 1   \* first chunk will receive seqId 1
  /\ completedSeqId      = 0   \* 0 = no chunk completed yet
  /\ writeIdx            = 0
  /\ encodeIdx           = 0
  /\ subCBChainLength    = [s \in Slots  |-> 0]
  /\ subCBProgress       = [s \in Slots  |-> 0]
  /\ finalCommitted      = [s \in Slots  |-> FALSE]
  /\ slotIsPresent       = [s \in Slots  |-> FALSE]
  /\ presentTokenSignaled = [n \in SeqIds |-> FALSE]

(* ================================================================
   Actions
   ================================================================ *)

(*
 * WineBeginWrite
 * Wine thread claims the current write slot (must be Free).
 * Sets it to Writing to signal ownership.
 *)
WineBeginWrite ==
  /\ state[writeIdx] = "Free"
  /\ state' = [state EXCEPT ![writeIdx] = "Writing"]
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx,
                 subCBChainLength, subCBProgress, finalCommitted, slotIsPresent,
                 presentTokenSignaled>>

(*
 * WineCommit
 * Wine thread seals the current chunk and hands it to EncodeThread.
 *
 * Disabled (back-pressure) when InflightCount >= MAX_INFLIGHT.
 * This is the only synchronization point between WineThread and the pipeline.
 * Assigns a monotonically increasing seqId and advances writeIdx.
 *
 * Per R-BACK-2.29 / R-BACK-2.31, the encode thread will produce a chain of
 * 1..MAX_SUBCB sub-MTLCommandBuffers for this chunk. The chain length is
 * deterministic w.r.t. the chunk's record content; we model this by picking
 * a chain length nondeterministically at admit time and freezing it for the
 * lifetime of the chunk.
 *
 * Per R-BACK-2.30, a chunk may also be present-bearing. We model that as a
 * nondeterministic per-chunk boolean.
 *)
WineCommit ==
  /\ state[writeIdx] = "Writing"
  /\ InflightCount < MAX_INFLIGHT         \* back-pressure gate
  /\ currentSeqId <= MAX_SEQID            \* model-checking bound
  /\ \E chainLen \in SubCBLengths :
     \E isPresent \in BOOLEAN :
       /\ state'             = [state            EXCEPT ![writeIdx] = "Pending"]
       /\ chunkSeqId'        = [chunkSeqId       EXCEPT ![writeIdx] = currentSeqId]
       /\ subCBChainLength'  = [subCBChainLength EXCEPT ![writeIdx] = chainLen]
       /\ subCBProgress'     = [subCBProgress    EXCEPT ![writeIdx] = 0]
       /\ finalCommitted'    = [finalCommitted   EXCEPT ![writeIdx] = FALSE]
       /\ slotIsPresent'     = [slotIsPresent    EXCEPT ![writeIdx] = isPresent]
       /\ currentSeqId'      = currentSeqId + 1
       /\ writeIdx'          = (writeIdx + 1) % RING_SIZE
       /\ UNCHANGED <<completedSeqId, encodeIdx, presentTokenSignaled>>

(*
 * EncodeBegin
 * EncodeThread picks up the next Pending chunk and begins encoding.
 * No sub-CB has been committed yet (subCBProgress[encodeIdx] is still 0).
 *)
EncodeBegin ==
  /\ state[encodeIdx] = "Pending"
  /\ state' = [state EXCEPT ![encodeIdx] = "Encoding"]
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx,
                 subCBChainLength, subCBProgress, finalCommitted, slotIsPresent,
                 presentTokenSignaled>>

(*
 * EncodeMidChunkCommit (R-BACK-2.29)
 * EncodeThread closes a sub-CB in the middle of the chunk's chain and opens
 * the next one on the same MTLCommandQueue. The slot stays in Encoding —
 * mid-chunk sub-CB completion does NOT advance completedSeqId.
 * Enabled only when the chain has at least one more sub-CB after this one
 * (i.e. subCBProgress < chainLen - 1).
 *)
EncodeMidChunkCommit ==
  /\ state[encodeIdx] = "Encoding"
  /\ subCBProgress[encodeIdx] < subCBChainLength[encodeIdx] - 1
  /\ subCBProgress' = [subCBProgress EXCEPT ![encodeIdx] = subCBProgress[encodeIdx] + 1]
  /\ UNCHANGED <<state, chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx,
                 subCBChainLength, finalCommitted, slotIsPresent,
                 presentTokenSignaled>>

(*
 * EncodeFinalCommit (R-BACK-2.29 / R-BACK-2.30)
 * EncodeThread closes the final sub-CB in the chain and submits it. Only the
 * final sub-CB carries present metadata (R-BACK-2.30); the slot transitions
 * to GPU and is marked finalCommitted so FinishComplete can later advance
 * completedSeqId for this seqId.
 *
 * Enabled when subCBProgress is at the chain's last index (chainLen - 1).
 *)
EncodeFinalCommit ==
  /\ state[encodeIdx] = "Encoding"
  /\ subCBProgress[encodeIdx] = subCBChainLength[encodeIdx] - 1
  /\ state'           = [state          EXCEPT ![encodeIdx] = "GPU"]
  /\ subCBProgress'   = [subCBProgress  EXCEPT ![encodeIdx] = subCBChainLength[encodeIdx]]
  /\ finalCommitted'  = [finalCommitted EXCEPT ![encodeIdx] = TRUE]
  /\ encodeIdx'       = (encodeIdx + 1) % RING_SIZE
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx,
                 subCBChainLength, slotIsPresent, presentTokenSignaled>>

(*
 * FinishComplete (R-BACK-2.29 / R-BACK-2.30 / R-BACK-2.32)
 * GPU signals completion for the oldest submitted command buffer chain.
 * Metal guarantees same-queue command buffers complete in submission order
 * (FIFO across both chunks and sub-CBs within a chunk), so we complete the
 * GPU slot with the smallest chunkSeqId. Because the slot's chain has already
 * advanced through every EncodeMidChunkCommit and finally EncodeFinalCommit
 * (sequential per-slot transitions in this model), reaching the GPU state
 * implies all sub-CBs in the chain are committed.
 *
 * Sets completedSeqId to the just-completed chunk's seqId (R-BACK-2.32). If
 * the chunk was present-bearing, the present token for that seqId rises here
 * (R-BACK-2.30): only the final sub-CB completion can fire the token.
 *)
FinishComplete ==
  \E s \in Slots :
    /\ state[s] = "GPU"
    /\ \A s2 \in Slots :   \* s has the smallest seqId — Metal in-order guarantee
         state[s2] = "GPU" => chunkSeqId[s] <= chunkSeqId[s2]
    /\ state'             = [state            EXCEPT ![s] = "Free"]
    /\ chunkSeqId'        = [chunkSeqId       EXCEPT ![s] = 0]
    /\ subCBChainLength'  = [subCBChainLength EXCEPT ![s] = 0]
    /\ subCBProgress'     = [subCBProgress    EXCEPT ![s] = 0]
    /\ finalCommitted'    = [finalCommitted   EXCEPT ![s] = FALSE]
    /\ slotIsPresent'     = [slotIsPresent    EXCEPT ![s] = FALSE]
    /\ completedSeqId'    = chunkSeqId[s]
    /\ presentTokenSignaled' =
         IF slotIsPresent[s]
         THEN [presentTokenSignaled EXCEPT ![chunkSeqId[s]] = TRUE]
         ELSE presentTokenSignaled
    /\ UNCHANGED <<currentSeqId, writeIdx, encodeIdx>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ WineBeginWrite
  \/ WineCommit
  \/ EncodeBegin
  \/ EncodeMidChunkCommit
  \/ EncodeFinalCommit
  \/ FinishComplete

(*
 * Fairness assumptions:
 *   WF on EncodeThread (begin/mid/final) and FinishThread — they always
 *   make progress when they have work available. WineThread fairness is NOT
 *   required; the application may stop issuing draw calls at any time.
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(EncodeBegin)
  /\ WF_vars(EncodeMidChunkCommit)
  /\ WF_vars(EncodeFinalCommit)
  /\ WF_vars(FinishComplete)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ state                \in [Slots  -> ChunkStates]
  /\ chunkSeqId           \in [Slots  -> Nat]
  /\ currentSeqId         \in Nat
  /\ completedSeqId       \in Nat
  /\ writeIdx             \in Slots
  /\ encodeIdx            \in Slots
  /\ subCBChainLength     \in [Slots  -> 0 .. MAX_SUBCB]
  /\ subCBProgress        \in [Slots  -> 0 .. MAX_SUBCB]
  /\ finalCommitted       \in [Slots  -> BOOLEAN]
  /\ slotIsPresent        \in [Slots  -> BOOLEAN]
  /\ presentTokenSignaled \in [SeqIds -> BOOLEAN]

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * SeqIdSafety
 * Completed work can never exceed submitted work.
 * completedSeqId is always strictly less than currentSeqId
 * (currentSeqId is the NEXT id to assign, not the last assigned).
 *)
SeqIdSafety ==
  completedSeqId < currentSeqId

(*
 * BoundedInflight
 * Back-pressure keeps the number of simultaneously in-flight chunks within
 * the configured MAX_INFLIGHT limit.
 *)
BoundedInflight ==
  InflightCount <= MAX_INFLIGHT

(*
 * RingSafety
 * The write slot must only ever be Free (waiting to be claimed) or Writing
 * (currently being filled). It must NEVER be Pending, Encoding, or GPU —
 * that would mean the Wine thread's write index has lapped an in-flight slot,
 * corrupting its contents.
 *)
RingSafety ==
  state[writeIdx] \in {"Free", "Writing"}

(*
 * EncodeSafety
 * The encode head points to the next slot the EncodeThread will process.
 * It must never point to a GPU slot — that would mean encodeIdx has lapped
 * a slot still waiting for GPU completion.
 *)
EncodeSafety ==
  state[encodeIdx] \in {"Free", "Writing", "Pending", "Encoding"}

(*
 * SubCBProgressBounded (R-BACK-2.29)
 * Sub-CB progress per slot is bounded by the chain length chosen at admit.
 * For Free slots both are 0; for in-flight slots progress is in 0..chainLen
 * with a strict invariant that mid-chunk transitions cannot exceed the chain.
 *)
SubCBProgressBounded ==
  \A s \in Slots :
    /\ subCBProgress[s] \in 0 .. subCBChainLength[s]
    /\ subCBChainLength[s] \in 0 .. MAX_SUBCB
    /\ (state[s] = "Free"     => subCBChainLength[s] = 0 /\ subCBProgress[s] = 0)
    /\ (state[s] = "Writing"  => subCBChainLength[s] = 0 /\ subCBProgress[s] = 0)
    /\ (state[s] = "Pending"  => subCBChainLength[s] >= 1 /\ subCBProgress[s] = 0)
    /\ (state[s] = "Encoding" => subCBChainLength[s] >= 1
                                  /\ subCBProgress[s] < subCBChainLength[s])
    /\ (state[s] = "GPU"      => subCBProgress[s] = subCBChainLength[s])

(*
 * OnlyFinalAdvancesSeqId (R-BACK-2.29 / R-BACK-2.32)
 * completedSeqId only ever names a chunk whose chain reached EncodeFinalCommit.
 * Equivalently, a slot's seqId can only become completed after the slot has
 * been finally-committed (transitioned to GPU). Mid-chunk sub-CBs never
 * advance completedSeqId.
 *
 * Every slot still carrying a non-zero seqId must be GPU-side iff finalCommitted,
 * Encoding/Pending iff not finalCommitted (mid-chunk progress doesn't flip the
 * flag).
 *)
OnlyFinalAdvancesSeqId ==
  /\ \A s \in Slots :
       /\ (state[s] \in {"Free", "Writing"}      => finalCommitted[s] = FALSE)
       /\ (state[s] \in {"Pending", "Encoding"}  => finalCommitted[s] = FALSE)
       /\ (state[s] = "GPU"                      => finalCommitted[s] = TRUE)
  /\ completedSeqId < currentSeqId

(*
 * PresentRoutedToTail (R-BACK-2.30)
 * The present-frame token rises only after the chain's last sub-CB completes.
 * Concretely: presentTokenSignaled[seq] = TRUE implies that seq has been
 * GPU-completed (seq <= completedSeqId), which by construction (only
 * FinishComplete advances completedSeqId, and only after EncodeFinalCommit on
 * that slot) means the final sub-CB has already run on the GPU.
 *
 * Additionally: if a slot currently holds a seqId that has been
 * present-token-signaled, it must be in GPU/Free with finalCommitted reflecting
 * that the chain finished. (This rules out a token rising while the slot is
 * still mid-chunk.)
 *)
PresentRoutedToTail ==
  /\ \A seq \in SeqIds :
       presentTokenSignaled[seq] => seq <= completedSeqId
  /\ \A s \in Slots :
       (state[s] \in {"Pending", "Encoding"}
        /\ chunkSeqId[s] \in SeqIds)
       => presentTokenSignaled[chunkSeqId[s]] = FALSE

(*
 * MonotonicCompletion
 * completedSeqId is monotonically non-decreasing (GPU completes in order).
 * This is a temporal safety property.
 *)
MonotonicCompletion ==
  [][completedSeqId' >= completedSeqId]_completedSeqId

(*
 * MonotonicPresentToken (R-BACK-2.30)
 * Once a present token rises, it stays raised for that seqId.
 *)
MonotonicPresentToken ==
  [][\A seq \in SeqIds :
       presentTokenSignaled[seq] => presentTokenSignaled'[seq]]_presentTokenSignaled

Safety ==
  /\ TypeOK
  /\ SeqIdSafety
  /\ BoundedInflight
  /\ RingSafety
  /\ EncodeSafety
  /\ SubCBProgressBounded
  /\ OnlyFinalAdvancesSeqId
  /\ PresentRoutedToTail

(* ================================================================
   Liveness properties
   ================================================================ *)

(*
 * PendingEventuallyFree
 * Every chunk that reaches Pending state will eventually reach Free.
 * This guarantees the pipeline never permanently stalls — all submitted
 * work drains through encode → (sub-CB chain) → GPU → finish.
 *)
PendingEventuallyFree ==
  \A s \in Slots : state[s] = "Pending" ~> state[s] = "Free"

(*
 * EventuallyDrained
 * If the Wine thread stops committing new chunks (bounded by MAX_SEQID),
 * the GPU eventually processes all remaining work and the queue reaches
 * a quiescent state where completedSeqId + 1 = currentSeqId.
 *)
EventuallyDrained ==
  <>[](completedSeqId + 1 = currentSeqId)

====
