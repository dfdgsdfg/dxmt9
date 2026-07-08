---- MODULE ConcurrentProgressSignals ----
(*
 * dxmt9 Concurrent Progress Signals — TLA+ Specification
 *
 * Closes audit gap G1: pacing independence not formally verified across the
 * three architectural progress axes. Required by R-ARCH-6.8 and R-ARCH-6.9.
 *
 * The dxmt9 architecture exposes three independently advancing progress signals
 * (spec.md §6.5):
 *
 *   completedSeqId         — advanced by every command-buffer completion;
 *                            consumed by query resolution and readback waits.
 *   presentCompletedSeqId  — advanced only by present-bearing command-buffer
 *                            completion; consumed by frame-latency gates.
 *   ringSlotOccupancy      — bounded by chunk admission; consumed by queue
 *                            writer back-pressure.
 *
 * The architecture promises that a wait on any one of these signals does not
 * block progress on the other two beyond the ordering invariant
 *
 *     presentCompletedSeqId <= completedSeqId.
 *
 * This module models a deliberately minimal composite — it is NOT a refinement
 * of QuerySeqId.tla, PresentFrameLatency.tla, or CommandQueue.tla. Those
 * modules each prove their own axis-local properties; the composite would
 * explode the state space without adding cross-axis evidence. Here we keep
 * just the three counters, three wait kinds, and the three liveness
 * properties that make pacing independence checkable.
 *
 * Requirement traceability:
 *   R-ARCH-6.8  Pacing axes must be independent
 *   R-ARCH-6.9  Pacing independence must be observable
 *   spec.md §6.5  Pacing Independence (cross-axis non-blocking contract)
 *
 * Properties verified:
 *   Safety   — TypeOK, PacingOrdering, RingOccupancyBound,
 *              FrameLatencyBound, OutstandingAccounting
 *   Liveness — NoQueryWaitBlocksPresent,
 *              NoFrameLatencyBlocksQuery,
 *              NoRingPressureBlocksPresentCompletion
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  MaxRing,           \* ring-slot capacity (e.g. 3)
  MaxFrameLatency,   \* frame-latency token budget (e.g. 2)
  MaxSeqId,          \* model-checking bound on assigned seq IDs
  Queries            \* finite set of query identifiers (e.g. {q1, q2})

ASSUME MaxRing         \in Nat /\ MaxRing >= 1
ASSUME MaxFrameLatency \in Nat /\ MaxFrameLatency >= 1
ASSUME MaxFrameLatency <= MaxRing
ASSUME MaxSeqId        \in Nat /\ MaxSeqId >= MaxRing + 1
ASSUME IsFiniteSet(Queries)

QueryStates == {"Idle", "Issued", "Resolved"}

VARIABLES
  completedSeqId,          \* Nat — advanced by any command-buffer completion
  presentCompletedSeqId,   \* Nat — advanced only by present-bearing completion
  pendingNonPresent,       \* Nat — submitted-but-not-completed non-present chunks
  pendingPresent,          \* Nat — submitted-but-not-completed present chunks
  outstandingPresent,      \* Nat — same as pendingPresent (frame-latency gate view)
  nextSeqId,               \* Nat — seq ID that will be assigned to the next submission
  qState,                  \* FUNCTION Queries -> QueryStates
  qTargetSeqId,            \* FUNCTION Queries -> Nat (target for resolution; 0 if Idle)
  qStartPresent            \* FUNCTION Queries -> Nat (presentCompletedSeqId at issue time)

vars ==
  << completedSeqId,
     presentCompletedSeqId,
     pendingNonPresent,
     pendingPresent,
     outstandingPresent,
     nextSeqId,
     qState,
     qTargetSeqId,
     qStartPresent >>

ringSlotOccupancy == pendingNonPresent + pendingPresent
ringFull          == ringSlotOccupancy = MaxRing
frameLatencyFull  == outstandingPresent = MaxFrameLatency

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ completedSeqId        = 0
  /\ presentCompletedSeqId = 0
  /\ pendingNonPresent     = 0
  /\ pendingPresent        = 0
  /\ outstandingPresent    = 0
  /\ nextSeqId             = 1
  /\ qState                = [q \in Queries |-> "Idle"]
  /\ qTargetSeqId          = [q \in Queries |-> 0]
  /\ qStartPresent         = [q \in Queries |-> 0]

(* ================================================================
   Submission actions
   ================================================================ *)

(*
 * SubmitNonPresent
 * Queue accepts a non-present chunk. Requires a free ring slot but is NOT
 * gated by frame-latency. This action models any draw / readback / query END
 * carrying chunk that is not bound to a presentDrawable.
 *)
SubmitNonPresent ==
  /\ nextSeqId <= MaxSeqId
  /\ ringSlotOccupancy < MaxRing
  /\ pendingNonPresent'   = pendingNonPresent + 1
  /\ nextSeqId'           = nextSeqId + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  pendingPresent,
                  outstandingPresent,
                  qState,
                  qTargetSeqId,
                  qStartPresent >>

(*
 * SubmitPresent
 * Queue accepts a present-bearing chunk. Requires both a free ring slot AND
 * a free frame-latency token.
 *)
SubmitPresent ==
  /\ nextSeqId <= MaxSeqId
  /\ ringSlotOccupancy < MaxRing
  /\ outstandingPresent < MaxFrameLatency
  /\ pendingPresent'      = pendingPresent + 1
  /\ outstandingPresent'  = outstandingPresent + 1
  /\ nextSeqId'           = nextSeqId + 1
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  pendingNonPresent,
                  qState,
                  qTargetSeqId,
                  qStartPresent >>

(* ================================================================
   Completion actions
   ================================================================ *)

(*
 * CompleteNonPresent
 * GPU completes a non-present chunk. Advances completedSeqId only. Frees one
 * ring slot. Does NOT touch presentCompletedSeqId or outstandingPresent — the
 * present timeline is independent of non-present completion.
 *)
CompleteNonPresent ==
  /\ pendingNonPresent > 0
  /\ completedSeqId'    = completedSeqId + 1
  /\ pendingNonPresent' = pendingNonPresent - 1
  /\ UNCHANGED << presentCompletedSeqId,
                  pendingPresent,
                  outstandingPresent,
                  nextSeqId,
                  qState,
                  qTargetSeqId,
                  qStartPresent >>

(*
 * CompletePresent
 * GPU completes a present-bearing chunk. Advances both completedSeqId AND
 * presentCompletedSeqId, frees one ring slot AND one frame-latency token.
 * The combined advance is what preserves PacingOrdering.
 *)
CompletePresent ==
  /\ pendingPresent > 0
  /\ completedSeqId'        = completedSeqId + 1
  /\ presentCompletedSeqId' = presentCompletedSeqId + 1
  /\ pendingPresent'        = pendingPresent - 1
  /\ outstandingPresent'    = outstandingPresent - 1
  /\ UNCHANGED << pendingNonPresent,
                  nextSeqId,
                  qState,
                  qTargetSeqId,
                  qStartPresent >>

(* ================================================================
   Query actions
   ================================================================ *)

(*
 * IssueQuery(q)
 * Application issues a query END marker. The query is bound to the chunk that
 * will carry it — the next seqId to be assigned. Snapshot of
 * presentCompletedSeqId at issue time records what counts as "present made
 * progress past this query waiter" for the cross-axis liveness property.
 *)
IssueQuery(q) ==
  /\ qState[q] \in {"Idle", "Resolved"}
  /\ nextSeqId <= MaxSeqId
  /\ qState'        = [qState        EXCEPT ![q] = "Issued"]
  /\ qTargetSeqId'  = [qTargetSeqId  EXCEPT ![q] = nextSeqId]
  /\ qStartPresent' = [qStartPresent EXCEPT ![q] = presentCompletedSeqId]
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  pendingNonPresent,
                  pendingPresent,
                  outstandingPresent,
                  nextSeqId >>

(*
 * ResolveQuery(q)
 * Polling thread observes completedSeqId has reached the target chunk's
 * seq ID and transitions the query to Resolved.
 *)
ResolveQuery(q) ==
  /\ qState[q] = "Issued"
  /\ completedSeqId >= qTargetSeqId[q]
  /\ qState' = [qState EXCEPT ![q] = "Resolved"]
  /\ UNCHANGED << completedSeqId,
                  presentCompletedSeqId,
                  pendingNonPresent,
                  pendingPresent,
                  outstandingPresent,
                  nextSeqId,
                  qTargetSeqId,
                  qStartPresent >>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ SubmitNonPresent
  \/ SubmitPresent
  \/ CompleteNonPresent
  \/ CompletePresent
  \/ \E q \in Queries : IssueQuery(q)
  \/ \E q \in Queries : ResolveQuery(q)

(*
 * Fairness:
 *   Completion actions are weakly fair — if a completion is enabled it must
 *   eventually fire. ResolveQuery is weakly fair so a query that has reached
 *   its target seqId resolves. SubmitNonPresent and SubmitPresent are weakly
 *   fair so submissions are not starved while their gate is open; this is
 *   what powers the cross-axis liveness properties (a query waiter must not
 *   prevent presents from being submitted, and vice versa).
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(SubmitNonPresent)
  /\ WF_vars(SubmitPresent)
  /\ WF_vars(CompleteNonPresent)
  /\ WF_vars(CompletePresent)
  /\ \A q \in Queries : WF_vars(ResolveQuery(q))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ completedSeqId        \in 0 .. MaxSeqId
  /\ presentCompletedSeqId \in 0 .. MaxSeqId
  /\ pendingNonPresent     \in 0 .. MaxRing
  /\ pendingPresent        \in 0 .. MaxRing
  /\ outstandingPresent    \in 0 .. MaxFrameLatency
  /\ nextSeqId             \in 1 .. (MaxSeqId + 1)
  /\ qState                \in [Queries -> QueryStates]
  /\ qTargetSeqId          \in [Queries -> 0 .. MaxSeqId]
  /\ qStartPresent         \in [Queries -> 0 .. MaxSeqId]

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * PacingOrdering — the SOLE ordering invariant linking the three axes.
 * A present token can never advance ahead of the underlying command buffer
 * completion. spec.md §6.5 calls this out as the only relationship.
 *)
PacingOrdering ==
  presentCompletedSeqId <= completedSeqId

(*
 * Ring occupancy is bounded by MaxRing — back-pressure is enforced.
 *)
RingOccupancyBound ==
  ringSlotOccupancy <= MaxRing

(*
 * Frame-latency is bounded by MaxFrameLatency — present admission is gated.
 *)
FrameLatencyBound ==
  outstandingPresent <= MaxFrameLatency

(*
 * Bookkeeping: outstandingPresent agrees with pendingPresent. Submitted
 * chunks are accounted for in nextSeqId, completedSeqId, and the pending
 * counters.
 *)
OutstandingAccounting ==
  /\ outstandingPresent = pendingPresent
  /\ pendingNonPresent + pendingPresent = (nextSeqId - 1) - completedSeqId
  /\ \A q \in Queries :
       qState[q] = "Resolved" => completedSeqId >= qTargetSeqId[q]

Safety ==
  /\ TypeOK
  /\ PacingOrdering
  /\ RingOccupancyBound
  /\ FrameLatencyBound
  /\ OutstandingAccounting

(* ================================================================
   Liveness properties — the heart of T2
   ================================================================ *)

(*
 * NoQueryWaitBlocksPresent
 *
 * R-ARCH-6.8: a stalled query waiter must not delay present completion or
 * frame-token advance.
 *
 * Statement: from any state where a query is Issued, eventually either the
 * query resolves OR presentCompletedSeqId advances past the value it had
 * when the query was issued. In other words, an unresolved query never
 * pins the present axis.
 *
 * Holds because the present timeline is driven by SubmitPresent +
 * CompletePresent and neither action takes qState as a precondition.
 *)
NoQueryWaitBlocksPresent ==
  \A q \in Queries :
    (qState[q] = "Issued") ~>
      (qState[q] = "Resolved" \/ presentCompletedSeqId > qStartPresent[q])

(*
 * NoFrameLatencyBlocksQuery
 *
 * R-ARCH-6.8: a saturated frame-latency gate must not delay query resolution.
 *
 * Statement: from any state where the frame-latency budget is full AND a
 * query is Issued, eventually a query resolves. (The query may not be the
 * same one — any resolution shows the query axis is still moving.)
 *
 * Holds because non-present submissions and completions are not gated by
 * outstandingPresent, so completedSeqId can still advance past
 * qTargetSeqId[q] while outstandingPresent stays at MaxFrameLatency.
 *)
NoFrameLatencyBlocksQuery ==
  (frameLatencyFull /\ \E q \in Queries : qState[q] = "Issued")
    ~> (\E q \in Queries : qState[q] = "Resolved")

(*
 * NoRingPressureBlocksPresentCompletion
 *
 * R-ARCH-6.8: ring back-pressure must not delay present completion of
 * already-submitted present chunks.
 *
 * Statement: from any state where the ring is full AND a present chunk is
 * in flight, eventually presentCompletedSeqId advances past its current
 * value. The bounded universal makes the temporal formula well-defined.
 *
 * Holds because CompletePresent fires whenever pendingPresent > 0, and
 * fairness ensures it eventually does. Ring fullness does not gate
 * completions.
 *)
NoRingPressureBlocksPresentCompletion ==
  \A n \in 0 .. MaxSeqId :
    (ringFull /\ pendingPresent > 0 /\ presentCompletedSeqId = n)
      ~> (presentCompletedSeqId > n)

====
