---- MODULE PresentFrameLatency ----
(*
 * dxmt9 Present Frame Latency - TLA+ Specification
 *
 * Models the queue-owned frame-latency token path for present-bearing chunks.
 *
 * The model separates three timelines:
 *
 *   Commit            - chunk accepted by the backend queue and assigned seqId
 *   CommandComplete   - Metal command buffer carrying that chunk completed
 *   PresentComplete   - present-bearing token signaled after command completion
 *
 * Non-present chunks advance only the normal sequence timeline. Present-bearing
 * chunks additionally enter the outstanding-present queue. The application may
 * be placed in a present wait state when accepting another present would exceed
 * MAX_FRAME_LATENCY; it may return only after an older present token completes
 * or shutdown/stop has been requested.
 *
 * Requirement traceability:
 *   R-BACK-2.12  Present metadata exists only for present-bearing chunks
 *   R-BACK-2.13  Command queue owns lifecycle, sequence, and frame-token signal
 *   R-BACK-6.4   Present-bearing chunks receive monotonically increasing tokens
 *   R-BACK-6.5   Frame-latency waits target present command-buffer completion
 *   R-BACK-6.6   Encode progress and present completion are separate timelines
 *   R-BACK-6.7   MAX_FRAME_LATENCY bounds incomplete present-bearing tokens
 *
 * Properties verified:
 *   Safety   - TypeOK, SeqTimelineSafety, PresentCompletionSafety,
 *              OutstandingPresentBound, PresentQueueSafety,
 *              AppWaitReturnSafe
 *   Liveness - SubmittedPresentsEventuallyComplete, WaitEventuallyReturnsOrStops
 *)

EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
  MAX_SEQID,          \* model-checking upper bound on committed seq IDs
  MAX_FRAME_LATENCY   \* maximum accepted incomplete present-bearing tokens

ASSUME MAX_SEQID \in Nat /\ MAX_SEQID >= 2
ASSUME MAX_FRAME_LATENCY \in Nat /\ MAX_FRAME_LATENCY >= 1
ASSUME MAX_FRAME_LATENCY < MAX_SEQID

SeqIds == 1 .. MAX_SEQID
SeqId0 == 0 .. MAX_SEQID

AppStates == {"Running", "Waiting", "Stopped"}

VARIABLES
  currentSeqId,          \* Nat - next seq ID to assign
  lastCommittedSeqId,    \* Nat - most recent queue-accepted chunk seq ID
  lastEncodedSeqId,      \* Nat - most recent chunk submitted to Metal
  completedSeqId,        \* Nat - most recent command-buffer completion
  presentCompletedSeqId, \* Nat - most recent completed present-bearing seq ID
  presentSubmitted,      \* SUBSET SeqIds - all accepted present-bearing chunks
  presentEncoded,        \* SUBSET SeqIds - presents whose chunk reached Metal
  presentOutstanding,    \* SUBSET SeqIds - accepted presents not yet completed
  presentQueue,          \* Seq(SeqIds) - FIFO order for outstanding presents
  appState,              \* AppStates - application Present() wait state
  pendingPresent,        \* BOOLEAN - app is waiting to accept one present
  stop                   \* BOOLEAN - shutdown/teardown unblocks waits

vars ==
  << currentSeqId,
     lastCommittedSeqId,
     lastEncodedSeqId,
     completedSeqId,
     presentCompletedSeqId,
     presentSubmitted,
     presentEncoded,
     presentOutstanding,
     presentQueue,
     appState,
     pendingPresent,
     stop >>

QueueSet(q) ==
  {q[i] : i \in 1 .. Len(q)}

NoDuplicates(q) ==
  \A i, j \in 1 .. Len(q) :
    i # j => q[i] # q[j]

StrictlyIncreasing(q) ==
  \A i, j \in 1 .. Len(q) :
    i < j => q[i] < q[j]

OutstandingPresentCount ==
  Cardinality(presentOutstanding)

CanAcceptPresent ==
  /\ ~stop
  /\ currentSeqId <= MAX_SEQID
  /\ OutstandingPresentCount < MAX_FRAME_LATENCY

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ currentSeqId          = 1
  /\ lastCommittedSeqId    = 0
  /\ lastEncodedSeqId      = 0
  /\ completedSeqId        = 0
  /\ presentCompletedSeqId = 0
  /\ presentSubmitted      = {}
  /\ presentEncoded        = {}
  /\ presentOutstanding    = {}
  /\ presentQueue          = <<>>
  /\ appState              = "Running"
  /\ pendingPresent        = FALSE
  /\ stop                  = FALSE

(* ================================================================
   Commit / application-facing actions
   ================================================================ *)

(*
 * CommitNonPresent
 * Accepts a draw/blit/readback/etc. chunk. It receives a normal seqId but does
 * not allocate a frame-latency token.
 *)
CommitNonPresent ==
  /\ ~stop
  /\ appState = "Running"
  /\ currentSeqId <= MAX_SEQID
  /\ currentSeqId'       = currentSeqId + 1
  /\ lastCommittedSeqId' = currentSeqId
  /\ UNCHANGED << lastEncodedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSubmitted,
                  presentEncoded,
                  presentOutstanding,
                  presentQueue,
                  appState,
                  pendingPresent,
                  stop >>

(*
 * CommitPresent
 * Accepts a present-bearing chunk when doing so keeps the number of accepted
 * incomplete present tokens within MAX_FRAME_LATENCY.
 *)
CommitPresent ==
  /\ appState = "Running"
  /\ ~pendingPresent
  /\ CanAcceptPresent
  /\ currentSeqId'          = currentSeqId + 1
  /\ lastCommittedSeqId'    = currentSeqId
  /\ presentSubmitted'      = presentSubmitted \cup {currentSeqId}
  /\ presentOutstanding'    = presentOutstanding \cup {currentSeqId}
  /\ presentQueue'          = Append(presentQueue, currentSeqId)
  /\ UNCHANGED << lastEncodedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentEncoded,
                  appState,
                  pendingPresent,
                  stop >>

(*
 * BeginPresentWait
 * The application attempts Present() while the frame-latency gate is full.
 * The chunk is not accepted until an older present token completes.
 *)
BeginPresentWait ==
  /\ ~stop
  /\ appState = "Running"
  /\ ~pendingPresent
  /\ currentSeqId <= MAX_SEQID
  /\ OutstandingPresentCount >= MAX_FRAME_LATENCY
  /\ appState'       = "Waiting"
  /\ pendingPresent' = TRUE
  /\ UNCHANGED << currentSeqId,
                  lastCommittedSeqId,
                  lastEncodedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSubmitted,
                  presentEncoded,
                  presentOutstanding,
                  presentQueue,
                  stop >>

(*
 * CommitPendingPresent
 * Returns from an application present wait by accepting the delayed present.
 * This is enabled only after present-token completion opens the gate.
 *)
CommitPendingPresent ==
  /\ appState = "Waiting"
  /\ pendingPresent
  /\ CanAcceptPresent
  /\ currentSeqId'          = currentSeqId + 1
  /\ lastCommittedSeqId'    = currentSeqId
  /\ presentSubmitted'      = presentSubmitted \cup {currentSeqId}
  /\ presentOutstanding'    = presentOutstanding \cup {currentSeqId}
  /\ presentQueue'          = Append(presentQueue, currentSeqId)
  /\ appState'              = "Running"
  /\ pendingPresent'        = FALSE
  /\ UNCHANGED << lastEncodedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentEncoded,
                  stop >>

(*
 * Stop
 * Shutdown/teardown wakes application waits. Liveness properties allow stop as
 * the only reason a pending wait or incomplete present need not finish normally.
 *)
Stop ==
  /\ ~stop
  /\ stop'           = TRUE
  /\ appState'       = "Stopped"
  /\ pendingPresent' = FALSE
  /\ UNCHANGED << currentSeqId,
                  lastCommittedSeqId,
                  lastEncodedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSubmitted,
                  presentEncoded,
                  presentOutstanding,
                  presentQueue >>

(* ================================================================
   Encode / GPU / present-completion actions
   ================================================================ *)

(*
 * EncodeChunk
 * Encode thread dequeues and commits the next chunk to Metal. For present
 * chunks this records that presentDrawable was encoded, but it does not signal
 * the frame-latency token.
 *)
EncodeChunk ==
  /\ ~stop
  /\ lastEncodedSeqId < lastCommittedSeqId
  /\ LET s == lastEncodedSeqId + 1 IN
     /\ lastEncodedSeqId' = s
     /\ presentEncoded' =
          IF s \in presentSubmitted
          THEN presentEncoded \cup {s}
          ELSE presentEncoded
  /\ UNCHANGED << currentSeqId,
                  lastCommittedSeqId,
                  completedSeqId,
                  presentCompletedSeqId,
                  presentSubmitted,
                  presentOutstanding,
                  presentQueue,
                  appState,
                  pendingPresent,
                  stop >>

(*
 * CommandComplete
 * Finish thread observes command-buffer completion in submission order. This
 * advances only the normal command sequence timeline.
 *)
CommandComplete ==
  /\ ~stop
  /\ completedSeqId < lastEncodedSeqId
  /\ completedSeqId' = completedSeqId + 1
  /\ UNCHANGED << currentSeqId,
                  lastCommittedSeqId,
                  lastEncodedSeqId,
                  presentCompletedSeqId,
                  presentSubmitted,
                  presentEncoded,
                  presentOutstanding,
                  presentQueue,
                  appState,
                  pendingPresent,
                  stop >>

(*
 * PresentComplete
 * Signals the oldest outstanding present token only after its command buffer
 * has completed. This is the frame-latency fence signal.
 *)
PresentComplete ==
  /\ ~stop
  /\ Len(presentQueue) > 0
  /\ LET s == Head(presentQueue) IN
     /\ s \in presentEncoded
     /\ s <= completedSeqId
     /\ presentCompletedSeqId' = s
     /\ presentOutstanding'    = presentOutstanding \ {s}
     /\ presentQueue'          = Tail(presentQueue)
  /\ UNCHANGED << currentSeqId,
                  lastCommittedSeqId,
                  lastEncodedSeqId,
                  completedSeqId,
                  presentSubmitted,
                  presentEncoded,
                  appState,
                  pendingPresent,
                  stop >>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \/ CommitNonPresent
  \/ CommitPresent
  \/ BeginPresentWait
  \/ CommitPendingPresent
  \/ EncodeChunk
  \/ CommandComplete
  \/ PresentComplete
  \/ Stop

(*
 * Fairness:
 *   EncodeChunk and CommandComplete keep accepted work moving through Metal.
 *   PresentComplete ensures submitted present tokens signal once their command
 *   completion is visible.
 *   CommitPendingPresent models the application returning from a blocked
 *   Present() once the queue-owned gate opens.
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ WF_vars(EncodeChunk)
  /\ WF_vars(CommandComplete)
  /\ WF_vars(PresentComplete)
  /\ WF_vars(CommitPendingPresent)

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ currentSeqId \in 1 .. (MAX_SEQID + 1)
  /\ lastCommittedSeqId \in SeqId0
  /\ lastEncodedSeqId \in SeqId0
  /\ completedSeqId \in SeqId0
  /\ presentCompletedSeqId \in SeqId0
  /\ presentSubmitted \subseteq SeqIds
  /\ presentEncoded \subseteq SeqIds
  /\ presentOutstanding \subseteq SeqIds
  /\ presentQueue \in Seq(SeqIds)
  /\ appState \in AppStates
  /\ pendingPresent \in BOOLEAN
  /\ stop \in BOOLEAN

(* ================================================================
   Safety invariants
   ================================================================ *)

SeqTimelineSafety ==
  /\ lastCommittedSeqId = currentSeqId - 1
  /\ completedSeqId <= lastEncodedSeqId
  /\ lastEncodedSeqId <= lastCommittedSeqId
  /\ presentEncoded \subseteq presentSubmitted
  /\ \A s \in presentSubmitted : s <= lastCommittedSeqId
  /\ \A s \in presentEncoded : s <= lastEncodedSeqId
  /\ stop => appState = "Stopped"
  /\ appState = "Waiting" => pendingPresent

(*
 * presentCompletedSeqId never exceeds completedSeqId; a present token cannot
 * signal ahead of the command buffer that carried presentDrawable.
 *)
PresentCompletionSafety ==
  /\ presentCompletedSeqId <= completedSeqId
  /\ presentCompletedSeqId = 0 \/ presentCompletedSeqId \in presentSubmitted
  /\ presentOutstanding = {s \in presentSubmitted : s > presentCompletedSeqId}
  /\ \A s \in presentSubmitted :
       s \notin presentOutstanding => s <= completedSeqId

OutstandingPresentBound ==
  OutstandingPresentCount <= MAX_FRAME_LATENCY

PresentQueueSafety ==
  /\ QueueSet(presentQueue) = presentOutstanding
  /\ NoDuplicates(presentQueue)
  /\ StrictlyIncreasing(presentQueue)
  /\ \A s \in presentOutstanding : s > presentCompletedSeqId

Safety ==
  /\ TypeOK
  /\ SeqTimelineSafety
  /\ PresentCompletionSafety
  /\ OutstandingPresentBound
  /\ PresentQueueSafety

(* ================================================================
   Temporal safety properties
   ================================================================ *)

PresentCompletionActionSafe ==
  [][presentCompletedSeqId' > presentCompletedSeqId =>
       presentCompletedSeqId' <= completedSeqId']_vars

AppWaitReturnSafe ==
  [][(appState = "Waiting" /\ appState' = "Running") =>
       (Cardinality(presentOutstanding') <= MAX_FRAME_LATENCY \/ stop')]_vars

(* ================================================================
   Liveness properties
   ================================================================ *)

SubmittedPresentsEventuallyComplete ==
  \A s \in SeqIds :
    s \in presentSubmitted ~> (s \notin presentOutstanding \/ stop)

WaitEventuallyReturnsOrStops ==
  appState = "Waiting" ~> (appState = "Running" \/ stop)

====
