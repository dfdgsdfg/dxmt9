---- MODULE QuerySeqId ----
(*
 * dxmt9 Query Seq-ID Fence — TLA+ Specification
 *
 * Models the resolution mechanism for D3DQUERYTYPE_EVENT and
 * D3DQUERYTYPE_OCCLUSION queries in the deferred command queue.
 *
 * Because commands are encoded on a separate thread, query results are
 * not available until the GPU has processed the chunk that contains the
 * query's END marker. The sequence ID fence mechanism tracks this:
 *
 *   query.issuedSeqId  = seqId of the chunk that held Issue(D3DISSUE_END)
 *   completedSeqId     = seqId of the most recently GPU-completed chunk
 *   resolved           iff completedSeqId >= query.issuedSeqId
 *
 * Key concern: deadlock freedom for the common busy-wait pattern:
 *
 *   while (pQuery->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
 *
 * With D3DGETDATA_FLUSH, the core commits the current pending chunk to
 * ensure the END marker reaches the GPU. Without this flush, the Wine
 * thread's spin could wait forever on a chunk that was never submitted.
 *
 * Requirement traceability:
 *   R-CORE-8.1  D3DQUERYTYPE_EVENT must be supported
 *   R-CORE-8.2  D3DQUERYTYPE_OCCLUSION must be supported
 *   core/queries.md §2  Sequence ID fence mechanism
 *   core/queries.md §3  EVENT query; deadlock-free flush+spin
 *
 * Properties verified:
 *   Safety   — TypeOK, QueryResolutionSafety, SeqIdMonotone
 *   Liveness — QueriesEventuallyResolve, NoDeadlockOnFlushSpin
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  MAX_SEQID,   \* model-checking bound on sequence IDs
  MAX_QUERIES  \* number of concurrent query objects

ASSUME MAX_SEQID  \in Nat /\ MAX_SEQID >= 1
ASSUME MAX_QUERIES \in Nat /\ MAX_QUERIES >= 1

QueryIds == 1 .. MAX_QUERIES

QueryStates == {"Idle", "Issued", "Resolved"}

(*
 * FlushPending models whether the Wine thread has called GetData with
 * D3DGETDATA_FLUSH for a particular query, triggering a CommitChunk.
 * This is the mechanism that prevents deadlock in the busy-wait pattern.
 *)

VARIABLES
  qState,          \* FUNCTION QueryIds → QueryStates
  qIssuedSeqId,    \* FUNCTION QueryIds → Nat  (0 if Idle/Resolved)
  currentSeqId,    \* Nat — seqId that will be assigned to the NEXT committed chunk
  completedSeqId,  \* Nat — seqId of most recently GPU-completed chunk (0 = none)
  pendingFlush     \* BOOLEAN — a FLUSH has been requested but not yet processed

vars == <<qState, qIssuedSeqId, currentSeqId, completedSeqId, pendingFlush>>

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ qState        = [q \in QueryIds |-> "Idle"]
  /\ qIssuedSeqId  = [q \in QueryIds |-> 0]
  /\ currentSeqId  = 1    \* next chunk seqId to assign
  /\ completedSeqId = 0   \* 0 = nothing completed
  /\ pendingFlush  = FALSE

(* ================================================================
   Actions
   ================================================================ *)

(*
 * IssueQuery(q)
 * Application calls Issue(D3DISSUE_END) on query q.
 * Records the current chunk's seqId as the issued seqId.
 * The query transitions from Idle (or Resolved) → Issued.
 *)
IssueQuery(q) ==
  /\ qState[q] \in {"Idle", "Resolved"}
  /\ currentSeqId <= MAX_SEQID
  /\ qState'       = [qState      EXCEPT ![q] = "Issued"]
  /\ qIssuedSeqId' = [qIssuedSeqId EXCEPT ![q] = currentSeqId]
  /\ UNCHANGED <<currentSeqId, completedSeqId, pendingFlush>>

(*
 * CommitChunk
 * Wine thread (or FLUSH path) commits the current chunk and advances currentSeqId.
 * After this, the chunk with seqId = old(currentSeqId) is in the GPU pipeline.
 *)
CommitChunk ==
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ pendingFlush' = FALSE   \* flush request satisfied
  /\ UNCHANGED <<qState, qIssuedSeqId, completedSeqId>>

(*
 * GPUComplete
 * GPU signals completion for the next pending chunk (in-order).
 * Advances completedSeqId by one step.
 * Precondition: there is at least one committed-but-not-completed chunk.
 *)
GPUComplete ==
  /\ completedSeqId < currentSeqId - 1
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED <<qState, qIssuedSeqId, currentSeqId, pendingFlush>>

(*
 * GetDataSFalse(q)
 * Application polls query q; GPU has not yet completed the issuing chunk.
 * Returns S_FALSE. If FLUSH is set, commits the current chunk.
 *)
GetDataFlush(q) ==
  /\ qState[q] = "Issued"
  /\ completedSeqId < qIssuedSeqId[q]   \* not yet resolved
  /\ pendingFlush' = TRUE                \* arm the flush
  /\ UNCHANGED <<qState, qIssuedSeqId, currentSeqId, completedSeqId>>

(*
 * GetDataSOK(q)
 * Application polls query q; GPU HAS completed the issuing chunk.
 * Returns S_OK; transitions query to Resolved.
 * This is only enabled when completedSeqId >= issuedSeqId.
 *)
GetDataSOK(q) ==
  /\ qState[q] = "Issued"
  /\ completedSeqId >= qIssuedSeqId[q]   \* resolution condition
  /\ qState' = [qState EXCEPT ![q] = "Resolved"]
  /\ UNCHANGED <<qIssuedSeqId, currentSeqId, completedSeqId, pendingFlush>>

(*
 * FlushCommit
 * When pendingFlush is set, the Wine thread commits any uncommitted chunk
 * to ensure the END marker reaches the GPU pipeline.
 * Models the D3DGETDATA_FLUSH mechanism that prevents deadlock.
 *)
FlushCommit ==
  /\ pendingFlush
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId' = currentSeqId + 1
  /\ pendingFlush' = FALSE
  /\ UNCHANGED <<qState, qIssuedSeqId, completedSeqId>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ CommitChunk
  \/ GPUComplete
  \/ FlushCommit
  \/ \E q \in QueryIds : IssueQuery(q)
  \/ \E q \in QueryIds : GetDataFlush(q)
  \/ \E q \in QueryIds : GetDataSOK(q)

(*
 * Fairness:
 *   WF(GPUComplete)     — GPU always eventually completes submitted work
 *   WF(GetDataSOK(q))   — core returns S_OK once the GPU is done
 *   WF(FlushCommit)     — flush requests are always honoured
 *
 * WineThread CommitChunk is NOT assumed fair: the application might stop
 * submitting draws at any time. Liveness holds via the flush mechanism.
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(GPUComplete)
  /\ WF_vars(FlushCommit)
  /\ \A q \in QueryIds : WF_vars(GetDataFlush(q)) /\ WF_vars(GetDataSOK(q))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ qState        \in [QueryIds -> QueryStates]
  /\ qIssuedSeqId  \in [QueryIds -> Nat]
  /\ currentSeqId  \in Nat
  /\ completedSeqId \in Nat
  /\ pendingFlush  \in BOOLEAN

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * QueryResolutionSafety
 * A query must NEVER be marked Resolved before the GPU has completed the
 * chunk that contained its END marker.
 *
 * This is the core correctness property: an application reading query results
 * when completedSeqId < issuedSeqId would see stale/wrong data.
 *)
QueryResolutionSafety ==
  \A q \in QueryIds :
    qState[q] = "Resolved" => completedSeqId >= qIssuedSeqId[q]

(*
 * SeqIdMonotone
 * completedSeqId never decreases and never overtakes submitted work.
 *)
SeqIdMonotone ==
  /\ completedSeqId < currentSeqId          \* never ahead of submissions
  /\ [][completedSeqId' >= completedSeqId]_completedSeqId  \* non-decreasing

Safety == TypeOK /\ QueryResolutionSafety

(* ================================================================
   Liveness
   ================================================================ *)

(*
 * QueriesEventuallyResolve
 * Every issued query eventually resolves.
 *
 * This holds because:
 *   1. IssueQuery captures currentSeqId as issuedSeqId.
 *   2. Either CommitChunk or FlushCommit advances currentSeqId past issuedSeqId.
 *   3. WF(GPUComplete) advances completedSeqId to issuedSeqId.
 *   4. WF(GetDataSOK) then transitions the query to Resolved.
 *   5. WF(GetDataFlush) ensures the polling path eventually arms the flush.
 *
 * The WF(FlushCommit) assumption models the D3DGETDATA_FLUSH guarantee:
 * the flush always eventually commits the pending chunk, preventing deadlock.
 *)
QueriesEventuallyResolve ==
  \A q \in QueryIds :
    qState[q] = "Issued" ~> qState[q] = "Resolved"

(*
 * NoDeadlockOnFlushSpin
 * The busy-wait pattern:
 *   while (GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
 * does not deadlock: if the query is Issued and FLUSH is armed, the
 * system eventually reaches a state where the query is Resolved.
 *
 * This follows from QueriesEventuallyResolve plus WF(FlushCommit).
 * Named explicitly to match the requirement in core/queries.md §3.
 *)
NoDeadlockOnFlushSpin ==
  \A q \in QueryIds :
    (qState[q] = "Issued" /\ pendingFlush) ~> qState[q] = "Resolved"

====
