---- MODULE QueryReplayReceipt ----
EXTENDS Naturals, TLC

(******************************************************************************
 * One captured polling attempt, bounded FIFO raws, one final Issue summary per
 * query-bearing raw (possibly the last of several BEGIN/END records). Raw
 * ordinals abstract qualified identities, not backend Metal sequence numbers.
 * Publish models Issue snapshot publication; Complete models the whole raw,
 * including its suffix. Only Complete authorizes copying the immutable value.
 * Later unrelated raws do not change the query target. New query admission
 * before copy supersedes; after copy it cannot redirect the retained value.
 *
 * Classify matches query::classifyReplayReceipt. Snapshot is the synchronized
 * visitReadyReceipt/cold-snapshot copy. Poll models pollQuerySnapshot and the
 * bridge's final terminal check. Ordinary and initializer completion are
 * distinct monotone abstractions; QueryInitializerFence owns actual upload
 * success tracking. No pixels, driver failure recovery or C++ memory model is
 * proved here. Fair CPU replay proves attempt termination, not bounded latency
 * or eventual GPU success. A pending GPU poll legitimately ends with S_FALSE.
 ******************************************************************************)

CONSTANTS MaxRaw, MaxEpoch, Mutation
VARIABLE s
vars == <<s>>

Init == s = [next |-> 0, admitted |-> 0, queries |-> {},
  published |-> 0, replayed |-> 0, liveQuery |-> 0, liveEpoch |-> 1,
  epoch |-> 1, terminal |-> FALSE, ordinary |-> 0, initializer |-> 0,
  phase |-> "Idle", captured |-> 0, captureOriginal |-> 0, retained |-> FALSE,
  snapshotTarget |-> 0, snapshotValue |-> 0, snapshotEpoch |-> 1,
  copyAdmitted |-> 0, copyReplayed |-> 0, result |-> "None",
  seenTerminal |-> FALSE, seenEpoch |-> 1,
  seenOrdinary |-> 0, seenInitializer |-> 0]

Classify(c, a, r, t) ==
  IF t \/ c > a \/ r > a THEN "Failed"
  ELSE IF c # a THEN "Superseded"
  ELSE IF r >= c THEN "Ready"
  ELSE "Pending"

(* Query completion is its last replayed query-bearing raw, not a device tail. *)
QueryReplayed ==
  LET done == {r \in s.queries : r <= s.replayed}
  IN IF done = {} THEN 0 ELSE CHOOSE r \in done : \A n \in done : n <= r

Admit(hasQuery) ==
  /\ ~s.terminal /\ s.next < MaxRaw
  /\ s' = [s EXCEPT !.next = @ + 1,
       !.admitted = IF hasQuery THEN s.next + 1 ELSE @,
       !.queries = IF hasQuery THEN @ \cup {s.next + 1} ELSE @]

Publish ==
  /\ ~s.terminal /\ s.published = s.replayed /\ s.published < s.next
  /\ LET r == s.published + 1
         q == r \in s.queries
     IN s' = [s EXCEPT !.published = r,
          !.liveQuery = IF q THEN r ELSE @,
          !.liveEpoch = IF q THEN s.epoch ELSE @,
          !.snapshotValue = IF Mutation = "MutableReceiptRedirection" /\
              q /\ s.snapshotTarget > 0 THEN r ELSE @]

Complete ==
  /\ ~s.terminal /\ s.replayed < s.published
  /\ s' = [s EXCEPT !.replayed = s.published]

Capture ==
  /\ ~s.terminal /\ s.phase = "Idle" /\ s.admitted > 0
  /\ s' = [s EXCEPT !.phase = "Captured", !.captured = s.admitted,
                    !.captureOriginal = s.admitted, !.retained = TRUE]

Snapshot ==
  /\ s.phase = "Captured" /\ ~s.terminal
  /\ IF Mutation = "MissingReplayCompletion"
        THEN s.admitted = s.captured /\ s.published >= s.captured
        ELSE IF Mutation = "IgnoreSuperseded"
          THEN QueryReplayed >= s.captured
          ELSE Classify(s.captured, s.admitted, QueryReplayed, s.terminal) = "Ready"
  /\ s' = [s EXCEPT !.phase = "Copied", !.snapshotTarget = s.liveQuery,
       !.snapshotValue = s.liveQuery, !.snapshotEpoch = s.liveEpoch,
       !.copyAdmitted = s.admitted, !.copyReplayed = QueryReplayed]

FinishWait ==
  /\ s.phase = "Captured"
  /\ Classify(s.captured, s.admitted, QueryReplayed, s.terminal)
       \in {"Failed", "Superseded"}
  /\ s' = [s EXCEPT !.phase = "Done",
       !.result = IF s.terminal THEN "Failed" ELSE "Pending"]

GpuOrdinary ==
  /\ ~s.terminal /\ s.ordinary < s.published
  /\ s' = [s EXCEPT !.ordinary = @ + 1]

GpuInitializer ==
  /\ ~s.terminal /\ s.initializer < s.published
  /\ s' = [s EXCEPT !.initializer = @ + 1]

Poll ==
  /\ s.phase = "Copied"
  /\ LET invalid == s.terminal \/ s.snapshotEpoch # s.epoch
         gpuReady == s.ordinary >= s.snapshotTarget /\
           (Mutation = "MissingGpuCompletion" \/ s.initializer >= s.snapshotTarget)
         result == IF invalid /\ Mutation # "IgnoreTerminalOrReset"
           THEN "Failed" ELSE IF gpuReady THEN "Ok" ELSE "Pending"
     IN s' = [s EXCEPT !.phase = "Done", !.result = result,
          !.seenTerminal = s.terminal, !.seenEpoch = s.epoch,
          !.seenOrdinary = s.ordinary, !.seenInitializer = s.initializer]

Reset ==
  /\ ~s.terminal /\ s.epoch < MaxEpoch
  /\ s' = [s EXCEPT !.epoch = @ + 1]

Fail == /\ ~s.terminal /\ s' = [s EXCEPT !.terminal = TRUE]

Next == Admit(TRUE) \/ Admit(FALSE) \/ Publish \/ Complete \/ Capture \/
        Snapshot \/ FinishWait \/ GpuOrdinary \/ GpuInitializer \/ Poll \/ Reset \/ Fail

Spec == Init /\ [][Next]_vars /\ WF_vars(Publish) /\ WF_vars(Complete) /\
        WF_vars(Snapshot) /\ WF_vars(FinishWait) /\ WF_vars(Poll)

TypeOK ==
  /\ s.next \in 0..MaxRaw /\ s.admitted \in 0..MaxRaw
  /\ s.queries \subseteq 1..MaxRaw
  /\ s.replayed \in 0..MaxRaw /\ s.published \in 0..MaxRaw
  /\ s.replayed <= s.published /\ s.published <= s.next
  /\ s.admitted <= s.next /\ s.liveQuery <= s.published
  /\ s.ordinary \in 0..s.published /\ s.initializer \in 0..s.published
  /\ s.epoch \in 1..MaxEpoch /\ s.liveEpoch \in 1..MaxEpoch
  /\ s.snapshotEpoch \in 1..MaxEpoch /\ s.terminal \in BOOLEAN
  /\ s.phase \in {"Idle", "Captured", "Copied", "Done"}
  /\ s.result \in {"None", "Pending", "Failed", "Ok"}
  /\ s.retained \in BOOLEAN /\ s.captured \in 0..MaxRaw
  /\ s.snapshotTarget \in 0..MaxRaw /\ s.snapshotValue \in 0..MaxRaw

NoCpuEarlySnapshot == s.snapshotTarget > 0 => s.copyReplayed >= s.captured
NoGpuEarlySuccess == s.result = "Ok" =>
  s.seenOrdinary >= s.snapshotTarget /\ s.seenInitializer >= s.snapshotTarget
SnapshotAdmissionIdentity == s.snapshotTarget > 0 =>
  s.snapshotTarget = s.captured /\ s.copyAdmitted = s.captured
PostSnapshotIssueStable == s.snapshotTarget > 0 => s.snapshotValue = s.snapshotTarget
NoFutureWaitExtension == s.captured > 0 => s.captured = s.captureOriginal
PollTargetFixed == s.snapshotTarget > 0 => s.snapshotTarget = s.captured
TerminalAndResetInvalidate == s.result = "Ok" =>
  ~s.seenTerminal /\ s.seenEpoch = s.snapshotEpoch
ReceiptRetained == s.captured > 0 => s.retained
ResolutionProgress == (s.phase = "Captured") ~> (s.phase = "Done")
====
