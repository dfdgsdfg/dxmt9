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
 *                  resulting MTLCommandBuffer to the GPU.
 *   FinishThread — waits for GPU completion signals; recycles chunk slots
 *                  back to Free; advances completedSeqId.
 *
 * Back-pressure: WineThread is blocked from committing a new chunk when
 * InflightCount >= MAX_INFLIGHT, preventing it from outpacing the GPU.
 *
 * Requirement traceability:
 *   R-BACK-2.1  Wine thread must not block on Metal API during draw submission
 *   R-BACK-2.2  Bounded queue; Wine thread outrun limit = MAX_INFLIGHT (default 3)
 *   R-BACK-2.3  Encode thread must not allocate on hot path (not modeled here)
 *
 * Properties verified:
 *   Safety   — TypeOK, SeqIdSafety, BoundedInflight, RingSafety, EncodeSafety
 *   Liveness — PendingEventuallyFree, EventuallyDrained
 *
 * How to run (TLC model checker):
 *   Use CommandQueue.cfg with RING_SIZE=4, MAX_INFLIGHT=2, MAX_SEQID=6.
 *   Full production values (32, 3) make the state space too large for TLC;
 *   the small model exercises all structural behaviors.
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  RING_SIZE,    \* slots in the ring (use 4 for TLC; 32 in production)
  MAX_INFLIGHT, \* max simultaneously in-flight chunks (use 2 for TLC; 3 in production)
  MAX_SEQID     \* model-checking upper bound on sequence IDs

ASSUME RING_SIZE \in Nat /\ RING_SIZE > 1
ASSUME MAX_INFLIGHT \in Nat /\ MAX_INFLIGHT >= 1
ASSUME MAX_INFLIGHT < RING_SIZE   \* ring must have slack beyond the inflight ceiling
ASSUME MAX_SEQID \in Nat /\ MAX_SEQID > MAX_INFLIGHT

Slots == 0 .. (RING_SIZE - 1)

(*
 * Slot lifecycle:
 *
 *   Free ──► Writing ──► Pending ──► Encoding ──► GPU ──► Free
 *             (Wine)      (Wine→     (Encode     (GPU→
 *                          Encode)    →GPU)       Finish)
 *)
ChunkStates == {"Free", "Writing", "Pending", "Encoding", "GPU"}

VARIABLES
  state,           \* FUNCTION Slots → ChunkStates
  chunkSeqId,      \* FUNCTION Slots → Nat  (seq ID assigned at commit; 0 otherwise)
  currentSeqId,    \* Nat — next seq ID to assign (starts at 1)
  completedSeqId,  \* Nat — seq ID of the most recently GPU-completed chunk (0 = none)
  writeIdx,        \* Slots — slot WineThread is currently filling / will fill next
  encodeIdx        \* Slots — next slot EncodeThread will process

vars == <<state, chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx>>

InflightCount ==
  Cardinality({s \in Slots : state[s] \in {"Pending", "Encoding", "GPU"}})

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ state          = [s \in Slots |-> "Free"]
  /\ chunkSeqId     = [s \in Slots |-> 0]
  /\ currentSeqId   = 1   \* first chunk will receive seqId 1
  /\ completedSeqId = 0   \* 0 = no chunk completed yet
  /\ writeIdx       = 0
  /\ encodeIdx      = 0

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
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx>>

(*
 * WineCommit
 * Wine thread seals the current chunk and hands it to EncodeThread.
 *
 * Disabled (back-pressure) when InflightCount >= MAX_INFLIGHT.
 * This is the only synchronization point between WineThread and the pipeline.
 * Assigns a monotonically increasing seqId and advances writeIdx.
 *)
WineCommit ==
  /\ state[writeIdx] = "Writing"
  /\ InflightCount < MAX_INFLIGHT         \* back-pressure gate
  /\ currentSeqId <= MAX_SEQID            \* model-checking bound
  /\ state'         = [state      EXCEPT ![writeIdx] = "Pending"]
  /\ chunkSeqId'    = [chunkSeqId EXCEPT ![writeIdx] = currentSeqId]
  /\ currentSeqId'  = currentSeqId + 1
  /\ writeIdx'      = (writeIdx + 1) % RING_SIZE
  /\ UNCHANGED <<completedSeqId, encodeIdx>>

(*
 * EncodeBegin
 * EncodeThread picks up the next Pending chunk and begins encoding.
 *)
EncodeBegin ==
  /\ state[encodeIdx] = "Pending"
  /\ state' = [state EXCEPT ![encodeIdx] = "Encoding"]
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx, encodeIdx>>

(*
 * EncodeSubmit
 * EncodeThread finishes encoding and commits the MTLCommandBuffer to the GPU.
 * Advances encodeIdx to the next slot.
 *)
EncodeSubmit ==
  /\ state[encodeIdx] = "Encoding"
  /\ state'      = [state EXCEPT ![encodeIdx] = "GPU"]
  /\ encodeIdx'  = (encodeIdx + 1) % RING_SIZE
  /\ UNCHANGED <<chunkSeqId, currentSeqId, completedSeqId, writeIdx>>

(*
 * FinishComplete
 * GPU signals completion for the oldest submitted command buffer.
 * Metal guarantees command buffers complete in submission order (FIFO), so we
 * complete the GPU slot with the smallest chunkSeqId.
 *
 * Sets completedSeqId to the just-completed chunk's seqId, making in-flight
 * resources with lastUsedSeqId <= that value safe to free.
 *)
FinishComplete ==
  \E s \in Slots :
    /\ state[s] = "GPU"
    /\ \A s2 \in Slots :   \* s has the smallest seqId — Metal in-order guarantee
         state[s2] = "GPU" => chunkSeqId[s] <= chunkSeqId[s2]
    /\ state'          = [state      EXCEPT ![s] = "Free"]
    /\ chunkSeqId'     = [chunkSeqId EXCEPT ![s] = 0]
    /\ completedSeqId' = chunkSeqId[s]
    /\ UNCHANGED <<currentSeqId, writeIdx, encodeIdx>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ WineBeginWrite
  \/ WineCommit
  \/ EncodeBegin
  \/ EncodeSubmit
  \/ FinishComplete

(*
 * Fairness assumptions:
 *   WF on EncodeThread and FinishThread — they are always making progress
 *   when they have work available. WineThread fairness is NOT required;
 *   the application may stop issuing draw calls at any time.
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(EncodeBegin)
  /\ WF_vars(EncodeSubmit)
  /\ WF_vars(FinishComplete)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ state          \in [Slots -> ChunkStates]
  /\ chunkSeqId     \in [Slots -> Nat]
  /\ currentSeqId   \in Nat
  /\ completedSeqId \in Nat
  /\ writeIdx       \in Slots
  /\ encodeIdx      \in Slots

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
 * MonotonicCompletion
 * completedSeqId is monotonically non-decreasing (GPU completes in order).
 * This is a temporal safety property.
 *)
MonotonicCompletion ==
  [][completedSeqId' >= completedSeqId]_completedSeqId

Safety == TypeOK /\ SeqIdSafety /\ BoundedInflight /\ RingSafety /\ EncodeSafety

(* ================================================================
   Liveness properties
   ================================================================ *)

(*
 * PendingEventuallyFree
 * Every chunk that reaches Pending state will eventually reach Free.
 * This guarantees the pipeline never permanently stalls — all submitted
 * work drains through encode → GPU → finish.
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
