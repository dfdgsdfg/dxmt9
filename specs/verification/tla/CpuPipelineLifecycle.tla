---- MODULE CpuPipelineLifecycle ----
(******************************************************************************
 * R-BACK-2.88 / R-VERIF-2.23 production-owner closure.
 *
 * This is intentionally a small control/data-plane composition rather than a
 * second queue implementation.  Every source carries a generation, owner
 * disposition, and the admission wake generation it observed.  GPU completion
 * and Present settlement are performed by CompleteGpu, matching the
 * production observer; Reset and Teardown are control actions and cannot be
 * used to manufacture a GPU milestone.  PresentDiscipline is "Enforced" in
 * production and "Removed" only in the expected early-reclaim counterexample.
 ******************************************************************************)

EXTENDS Naturals, FiniteSets, TLC, PipelineLifecycleTable

CONSTANTS MaxSources, Capacity, RequiredParts, PresentSources,
          ParallelSources, SelectedSources, InlineSources, FailureSources,
          DirectSources, AllowReset, AllowTeardown,
          WakeDiscipline, ResetDiscipline,
          InlineDiscipline, PresentDiscipline,
          OwnerDiscipline, ReceiptDiscipline, ImportDiscipline,
          DeviceLossDiscipline

Sources == 1..MaxSources
Stages == {"Absent", "ProducerOwned", "RawOwned", "ReplayBorrowed",
           "FinalOwned", "Encoding", "GPUInFlight", "Completed",
           "Reclaimed"}
OwnedStages == {"ReplayBorrowed", "FinalOwned", "Encoding", "GPUInFlight",
                "Completed"}
(* ParallelSources describes candidates; SelectedSources is the bounded
 * production decision after eligibility/proof gates. *)
ChildCount(s) == IF s \in SelectedSources THEN 2 ELSE 1
Oldest(set) == CHOOSE s \in set : \A t \in set : s <= t
LifecycleAllowed(from, to, ownerName, dispositionName, controlName) ==
  KnownLifecycleRow(from, to, ownerName, dispositionName, controlName)

(* Bounded completion-frontier composition.  CompleteGpu owns the GPU tail;
 * the finish thread advances its waterline separately.  This relation is the
 * model-side contract for the native FinishAdvance snapshot and intentionally
 * does not add a second queue implementation to this lifecycle model. *)
(* The actions abstract the queue mutex handoff: producer facts are published
 * before submission, and completion observes them after the matching
 * release/acquire edge. TLC checks this bounded value relation, not arbitrary
 * C++ atomic-order executions or an unbounded Metal execution. *)
CompletionFrontierContiguous(finishWaterline, completedQueueDepth, gpuTail) ==
  gpuTail = finishWaterline + completedQueueDepth

FinishAdvanceAction(gpuTailBefore, finishBefore, depthBefore,
                    gpuTailAfter, finishAfter, depthAfter) ==
  /\ CompletionFrontierContiguous(finishBefore, depthBefore, gpuTailBefore)
  /\ CompletionFrontierContiguous(finishAfter, depthAfter, gpuTailAfter)
  /\ gpuTailAfter = gpuTailBefore
  /\ finishAfter = finishBefore + 1
  /\ depthBefore = depthAfter + 1

FinishAdvanceInvariant(gpuTailBefore, finishBefore, depthBefore,
                       gpuTailAfter, finishAfter, depthAfter) ==
  FinishAdvanceAction(gpuTailBefore, finishBefore, depthBefore,
                      gpuTailAfter, finishAfter, depthAfter)
  => finishAfter <= gpuTailAfter

VARIABLES phase, generation, epoch, nextArrival, constructed, borrows,
          childTotal, joined, authority, completed, noGpu, presentPending,
          presentSettled, occupancy, waiting, observedWake, wake,
          completedSeq, presentSeq, gpuCompletedTailSeq,
          finishWaterlineSeq, completedQueueDepth, stopped, resetCount,
          owner, disposition

vars == <<phase, generation, epoch, nextArrival, constructed, borrows,
  childTotal, joined, authority, completed, noGpu, presentPending,
  presentSettled, occupancy, waiting, observedWake, wake, completedSeq,
  presentSeq, gpuCompletedTailSeq, finishWaterlineSeq, completedQueueDepth,
  stopped, resetCount, owner, disposition>>

frontierVars == <<gpuCompletedTailSeq, finishWaterlineSeq,
                  completedQueueDepth>>

Init ==
  /\ phase = [s \in Sources |-> "Absent"]
  /\ generation = [s \in Sources |-> 1]
  /\ epoch = 1
  /\ nextArrival = 1
  /\ constructed = [s \in Sources |-> 0]
  /\ borrows = [s \in Sources |-> 0]
  /\ childTotal = [s \in Sources |-> 0]
  /\ joined = [s \in Sources |-> 0]
  /\ authority = {}
  /\ completed = {}
  /\ noGpu = {}
  /\ presentPending = {}
  /\ presentSettled = {}
  /\ occupancy = 0
  /\ waiting = {}
  /\ observedWake = [s \in Sources |-> 0]
  /\ wake = 0
  /\ completedSeq = 0
  /\ presentSeq = 0
  /\ gpuCompletedTailSeq = 0
  /\ finishWaterlineSeq = 0
  /\ completedQueueDepth = 0
  /\ stopped = FALSE
  /\ resetCount = 0
  /\ owner = [s \in Sources |-> "None"]
  /\ disposition = [s \in Sources |-> "None"]

Arrive(s) ==
  /\ ~stopped /\ s = nextArrival /\ s \in Sources
  /\ LifecycleAllowed("SourceArrival", "ProducerOwned", "PeImport", "Advance", "Normal")
  /\ phase' = [phase EXCEPT ![s] = "ProducerOwned"]
  /\ owner' = [owner EXCEPT ![s] =
      IF ImportDiscipline = "Exact" THEN "PeImport" ELSE "None"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ nextArrival' = nextArrival + 1
  /\ UNCHANGED <<generation, epoch, constructed, borrows, childTotal,
      joined, authority, completed, noGpu, presentPending, presentSettled,
      occupancy, waiting, observedWake, wake, completedSeq, presentSeq,
      stopped, resetCount>>
  /\ UNCHANGED frontierVars

AdoptRaw(s) ==
  /\ ~stopped /\ phase[s] = "ProducerOwned"
  /\ LifecycleAllowed("ProducerOwned", "RawOwned", "Replay", "Advance", "Normal")
  /\ s = Oldest({t \in Sources : phase[t] = "ProducerOwned"})
  /\ phase' = [phase EXCEPT ![s] = "RawOwned"]
  /\ owner' = [owner EXCEPT ![s] = "Replay"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, occupancy, waiting, observedWake, wake, completedSeq,
      presentSeq, stopped, resetCount>>
  /\ UNCHANGED frontierVars

Park(s) ==
  /\ ~stopped /\ phase[s] = "RawOwned" /\ occupancy = Capacity
  /\ s = Oldest({t \in Sources : phase[t] = "RawOwned"})
  /\ s \notin waiting
  /\ waiting' = waiting \cup {s}
  /\ observedWake' = [observedWake EXCEPT ![s] = wake]
  /\ UNCHANGED <<phase, generation, epoch, nextArrival, constructed,
      borrows, childTotal, joined, authority, completed, noGpu,
      presentPending, presentSettled, occupancy, wake, completedSeq,
      presentSeq, stopped, resetCount, owner, disposition>>
  /\ UNCHANGED frontierVars

Admit(s) ==
  /\ ~stopped /\ phase[s] = "RawOwned" /\ occupancy < Capacity
  /\ LifecycleAllowed("RawOwned", "ReplayBorrowed", "Replay", "Advance", "Normal")
  /\ s = Oldest({t \in Sources : phase[t] = "RawOwned"})
  /\ s \notin waiting
  /\ phase' = [phase EXCEPT ![s] = "ReplayBorrowed"]
  /\ borrows' = [borrows EXCEPT ![s] = 1]
  /\ occupancy' = occupancy + 1
  /\ owner' = [owner EXCEPT ![s] = "Replay"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, childTotal,
      joined, authority, completed, noGpu, presentPending, presentSettled,
      waiting, observedWake, wake, completedSeq, presentSeq, stopped,
      resetCount>>
  /\ UNCHANGED frontierVars

Retry(s) ==
  /\ ~stopped /\ phase[s] = "RawOwned" /\ s \in waiting
  /\ LifecycleAllowed("RawOwned", "ReplayBorrowed", "Replay", "AdmissionRetry", "Normal")
  /\ s = Oldest({t \in Sources : phase[t] = "RawOwned"})
  /\ occupancy < Capacity /\ wake > observedWake[s]
  /\ phase' = [phase EXCEPT ![s] = "ReplayBorrowed"]
  /\ borrows' = [borrows EXCEPT ![s] = 1]
  /\ occupancy' = occupancy + 1
  /\ waiting' = waiting \ {s}
  /\ owner' = [owner EXCEPT ![s] = "Replay"]
  /\ disposition' = [disposition EXCEPT ![s] = "AdmissionRetry"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, childTotal,
      joined, authority, completed, noGpu, presentPending, presentSettled,
      observedWake, wake, completedSeq, presentSeq, stopped, resetCount>>
  /\ UNCHANGED frontierVars

Build(s) ==
  /\ ~stopped /\ phase[s] = "ReplayBorrowed"
  /\ LifecycleAllowed("ReplayBorrowed", "ReplayBorrowed", "Replay", "BuildProgress", "Normal")
  /\ constructed[s] < RequiredParts
  /\ constructed' = [constructed EXCEPT ![s] = @ + 1]
  /\ UNCHANGED <<phase, generation, epoch, nextArrival, borrows, childTotal,
      joined, authority, completed, noGpu, presentPending, presentSettled,
      occupancy, waiting, observedWake, wake, completedSeq, presentSeq,
      stopped, resetCount, owner, disposition>>
  /\ UNCHANGED frontierVars

ReturnReplayBorrow(s) ==
  /\ ~stopped /\ phase[s] = "ReplayBorrowed"
  /\ LifecycleAllowed("ReplayBorrowed", "ReplayBorrowed", "Replay", "Advance", "Normal")
  /\ constructed[s] = RequiredParts /\ borrows[s] = 1
  /\ borrows' = [borrows EXCEPT ![s] = 0]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<phase, generation, epoch, nextArrival, constructed,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, occupancy, waiting, observedWake, wake, completedSeq,
      presentSeq, stopped, resetCount, owner>>
  /\ UNCHANGED frontierVars

Publish(s) ==
  /\ ~stopped /\ phase[s] = "ReplayBorrowed"
  /\ (LifecycleAllowed("ReplayBorrowed", "FinalOwned", "DirectPublication", "Advance", "Normal")
      \/ LifecycleAllowed("ReplayBorrowed", "FinalOwned", "LegacyPublication", "Advance", "Normal"))
  /\ constructed[s] = RequiredParts /\ borrows[s] = 0
  /\ phase' = [phase EXCEPT ![s] = "FinalOwned"]
  /\ borrows' = [borrows EXCEPT ![s] = 0]
  /\ owner' = [owner EXCEPT ![s] =
        IF s \in DirectSources THEN "DirectPublication" ELSE "LegacyPublication"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, childTotal,
      joined, authority, completed, noGpu, presentPending, presentSettled,
      occupancy, waiting, observedWake, wake, completedSeq, presentSeq,
      stopped, resetCount>>
  /\ UNCHANGED frontierVars

BeginEncode(s) ==
  /\ ~stopped /\ phase[s] = "FinalOwned"
  /\ LifecycleAllowed("FinalOwned", "Encoding",
      IF s \in SelectedSources /\ OwnerDiscipline = "Exact"
      THEN "SelectedParallel" ELSE "SerialEncode",
      "Advance", "Normal")
  /\ phase' = [phase EXCEPT ![s] = "Encoding"]
  /\ borrows' = [borrows EXCEPT ![s] = ChildCount(s)]
  /\ childTotal' = [childTotal EXCEPT ![s] = ChildCount(s)]
  /\ joined' = [joined EXCEPT ![s] = 0]
  /\ owner' = [owner EXCEPT ![s] =
        IF s \in SelectedSources /\ OwnerDiscipline = "Exact"
        THEN "SelectedParallel" ELSE "SerialEncode"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, authority,
      completed, noGpu, presentPending, presentSettled, occupancy, waiting,
      observedWake, wake, completedSeq, presentSeq, stopped, resetCount>>
  /\ UNCHANGED frontierVars

Join(s) ==
  /\ ~stopped /\ phase[s] = "Encoding"
  /\ borrows[s] > 0 /\ joined[s] < childTotal[s]
  /\ borrows' = [borrows EXCEPT ![s] = @ - 1]
  /\ joined' = [joined EXCEPT ![s] = @ + 1]
  /\ disposition' = [disposition EXCEPT ![s] = "ChildJoin"]
  /\ UNCHANGED <<phase, generation, epoch, nextArrival, constructed,
      childTotal, authority, completed, noGpu, presentPending,
      presentSettled, occupancy, waiting, observedWake, wake, completedSeq,
      presentSeq, stopped, resetCount, owner>>
  /\ UNCHANGED frontierVars

Submit(s) ==
  /\ ~stopped /\ phase[s] = "Encoding" /\ joined[s] = childTotal[s]
  /\ LifecycleAllowed("Encoding", "GPUInFlight",
      IF s \in SelectedSources /\ OwnerDiscipline = "Exact"
      THEN "SelectedParallel" ELSE "Receipt", "Advance",
      IF s \in PresentSources THEN "Present" ELSE "Normal")
  /\ borrows[s] = 0
  /\ phase' = [phase EXCEPT ![s] = "GPUInFlight"]
  /\ authority' = IF ReceiptDiscipline = "Exact"
        THEN authority \cup {s} ELSE authority
  /\ owner' = [owner EXCEPT ![s] =
        IF s \in SelectedSources /\ OwnerDiscipline = "Exact"
        THEN "SelectedParallel" ELSE "Receipt"]
  /\ disposition' = [disposition EXCEPT ![s] = "Advance"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, completed, noGpu, presentPending, presentSettled,
      occupancy, waiting, observedWake, wake, completedSeq, presentSeq,
      stopped, resetCount>>
  /\ UNCHANGED frontierVars

CompleteGpu(s) ==
  /\ phase[s] = "GPUInFlight" /\ s \in authority
  /\ LifecycleAllowed("GPUInFlight", "Completed", "GpuCompletion", "Completed",
      IF s \in PresentSources THEN "Present" ELSE "Normal")
  /\ completedSeq = s - 1
  /\ CompletionFrontierContiguous(finishWaterlineSeq,
      completedQueueDepth, gpuCompletedTailSeq)
  /\ phase' = [phase EXCEPT ![s] = "Completed"]
  /\ completed' = completed \cup {s}
  /\ completedSeq' = s
  /\ presentPending' = IF s \in PresentSources
        THEN presentPending \ {s} ELSE presentPending
  /\ presentSettled' = IF s \in PresentSources /\ PresentDiscipline = "Enforced"
        THEN presentSettled \cup {s} ELSE presentSettled
  /\ presentSeq' = IF s \in PresentSources THEN s ELSE presentSeq
  /\ gpuCompletedTailSeq' = s
  /\ completedQueueDepth' = completedQueueDepth + 1
  /\ finishWaterlineSeq' = finishWaterlineSeq
  /\ owner' = [owner EXCEPT ![s] = "GpuCompletion"]
  /\ disposition' = [disposition EXCEPT ![s] = "Completed"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, noGpu, occupancy,
      waiting, observedWake, wake, stopped, resetCount>>

FinishAdvance(s) ==
  /\ ~stopped /\ phase[s] = "Completed" /\ s \in completed
  /\ s = finishWaterlineSeq + 1
  /\ completedQueueDepth > 0
  /\ CompletionFrontierContiguous(finishWaterlineSeq,
      completedQueueDepth, gpuCompletedTailSeq)
  /\ FinishAdvanceAction(gpuCompletedTailSeq, finishWaterlineSeq,
      completedQueueDepth, gpuCompletedTailSeq, finishWaterlineSeq + 1,
      completedQueueDepth - 1)
  /\ FinishAdvanceInvariant(gpuCompletedTailSeq, finishWaterlineSeq,
      completedQueueDepth, gpuCompletedTailSeq, finishWaterlineSeq + 1,
      completedQueueDepth - 1)
  /\ finishWaterlineSeq' = finishWaterlineSeq + 1
  /\ gpuCompletedTailSeq' = gpuCompletedTailSeq
  /\ completedQueueDepth' = completedQueueDepth - 1
  /\ UNCHANGED <<phase, generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, occupancy, waiting, observedWake, wake,
      completedSeq, presentSeq, stopped, resetCount, owner, disposition>>


Reclaim(s) ==
  /\ ~stopped /\ phase[s] = "Completed" /\ s \in completed
  /\ s <= finishWaterlineSeq
  /\ LifecycleAllowed("Completed", "Reclaimed", "Reclaim",
      IF s \in PresentSources THEN "PresentSettled" ELSE "Completed",
      IF s \in PresentSources THEN "Present" ELSE "Normal")
  /\ (s \notin PresentSources \/ s \in presentSettled \/
      PresentDiscipline = "Removed")
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ occupancy' = occupancy - 1
  /\ wake' = IF waiting # {} /\ WakeDiscipline = "Notify"
        THEN wake + 1 ELSE wake
  /\ owner' = [owner EXCEPT ![s] = "Reclaim"]
  /\ disposition' = [disposition EXCEPT ![s] =
        IF s \in PresentSources THEN "PresentSettled" ELSE "Completed"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, waiting, observedWake, completedSeq, presentSeq,
      stopped, resetCount>>
  /\ UNCHANGED frontierVars

InlineTerminal(s) ==
  /\ ~stopped /\ s \in InlineSources
  /\ phase[s] \in {"RawOwned", "FinalOwned", "Encoding"}
  /\ (LifecycleAllowed(phase[s], "Reclaimed", "Queue", "NoGpuTerminal", "Normal")
      \/ InlineDiscipline = "Fabricated")
  /\ borrows[s] = 0
  /\ phase' = [phase EXCEPT ![s] =
        IF InlineDiscipline = "Fabricated" THEN "GPUInFlight" ELSE "Reclaimed"]
  /\ noGpu' = noGpu \cup {s}
  /\ occupancy' = IF phase[s] = "RawOwned" THEN occupancy
        ELSE IF InlineDiscipline = "Fabricated" THEN occupancy ELSE occupancy - 1
  /\ wake' = IF waiting # {} /\ WakeDiscipline = "Notify"
        THEN wake + 1 ELSE wake
  /\ owner' = [owner EXCEPT ![s] = "Queue"]
  /\ disposition' = [disposition EXCEPT ![s] = "NoGpuTerminal"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, presentPending,
      presentSettled, waiting, observedWake, completedSeq, presentSeq,
      stopped, resetCount>>
  /\ UNCHANGED frontierVars

BridgeReject(s) ==
  /\ ~stopped /\ s \in FailureSources /\ phase[s] = "ProducerOwned"
  /\ LifecycleAllowed("ProducerOwned", "Reclaimed", "PeImport", "BridgeReject", "Exception")
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ owner' = [owner EXCEPT ![s] = "PeImport"]
  /\ disposition' = [disposition EXCEPT ![s] = "BridgeReject"]
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, occupancy, waiting, observedWake, wake, completedSeq,
      presentSeq, stopped, resetCount>>
  /\ UNCHANGED frontierVars

DeviceLoss ==
  /\ ~stopped /\ FailureSources # {}
  /\ \A s \in Sources :
      phase[s] = "Absent" \/ phase[s] = "Reclaimed" \/
      LifecycleAllowed(phase[s], "Reclaimed", "DeviceLoss", "FailStop",
                       "DeviceLoss")
  /\ stopped' = TRUE
  /\ phase' = [s \in Sources |->
      IF phase[s] = "Absent" \/ phase[s] = "Reclaimed"
      THEN phase[s] ELSE "Reclaimed"]
  /\ noGpu' = noGpu \cup {s \in Sources : phase[s] # "Absent"}
  /\ owner' = [s \in Sources |->
      IF phase[s] = "Absent" THEN owner[s]
      ELSE IF DeviceLossDiscipline = "Exact" THEN "DeviceLoss" ELSE "None"]
  /\ disposition' = [s \in Sources |->
      IF phase[s] = "Absent" THEN disposition[s]
      ELSE IF DeviceLossDiscipline = "Exact" THEN "FailStop" ELSE "None"]
  /\ occupancy' = 0
  /\ waiting' = {}
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, presentPending,
      presentSettled, observedWake, wake, completedSeq,
      presentSeq, resetCount>>
  /\ UNCHANGED frontierVars

Teardown ==
  /\ AllowTeardown /\ ~stopped
  /\ \A s \in Sources : phase[s] = "Absent" \/ phase[s] = "Reclaimed"
  /\ \A s \in Sources :
      phase[s] = "Absent" \/ phase[s] = "Reclaimed" \/
      LifecycleAllowed(phase[s], "Reclaimed", "Teardown", "Teardown",
                       "Teardown")
  /\ presentPending = {}
  /\ stopped' = TRUE
  /\ phase' = [s \in Sources |->
      IF phase[s] = "Absent" THEN "Absent" ELSE "Reclaimed"]
  /\ owner' = [s \in Sources |->
      IF phase[s] = "Absent" THEN owner[s] ELSE "Teardown"]
  /\ disposition' = [s \in Sources |->
      IF phase[s] = "Absent" THEN disposition[s] ELSE "Teardown"]
  /\ occupancy' = 0
  /\ waiting' = {}
  /\ UNCHANGED <<generation, epoch, nextArrival, constructed, borrows,
      childTotal, joined, authority, completed, noGpu, presentPending,
      presentSettled, observedWake, wake, completedSeq,
      presentSeq, resetCount>>
  /\ UNCHANGED frontierVars

Reset ==
  /\ AllowReset /\ stopped /\ resetCount = 0
  /\ \A s \in Sources : phase[s] = "Absent" \/ phase[s] = "Reclaimed"
  /\ epoch' = IF ResetDiscipline = "Advance" THEN epoch + 1 ELSE epoch
  /\ generation' = [s \in Sources |->
      IF ResetDiscipline = "Advance" THEN generation[s] + 1 ELSE generation[s]]
  /\ phase' = [s \in Sources |-> "Absent"]
  /\ nextArrival' = 1
  /\ stopped' = FALSE
  /\ resetCount' = resetCount + 1
  /\ constructed' = [s \in Sources |-> 0]
  /\ borrows' = [s \in Sources |-> 0]
  /\ childTotal' = [s \in Sources |-> 0]
  /\ joined' = [s \in Sources |-> 0]
  /\ authority' = {}
  /\ completed' = {}
  /\ noGpu' = {}
  /\ presentPending' = {}
  /\ presentSettled' = {}
  /\ occupancy' = 0
  /\ waiting' = {}
  /\ observedWake' = [s \in Sources |-> wake]
  /\ wake' = wake
  /\ completedSeq' = 0
  /\ presentSeq' = 0
  /\ gpuCompletedTailSeq' = 0
  /\ finishWaterlineSeq' = 0
  /\ completedQueueDepth' = 0
  /\ owner' = [s \in Sources |-> "None"]
  /\ disposition' = [s \in Sources |-> "None"]

Next ==
  \/ \E s \in Sources : Arrive(s) \/ AdoptRaw(s) \/ Park(s) \/ Admit(s)
  \/ \E s \in Sources : Retry(s) \/ Build(s) \/ ReturnReplayBorrow(s) \/ Publish(s) \/ BeginEncode(s)
  \/ \E s \in Sources : Join(s) \/ Submit(s) \/ CompleteGpu(s) \/ FinishAdvance(s)
  \/ \E s \in Sources : Reclaim(s)
  \/ \E s \in Sources : InlineTerminal(s) \/ BridgeReject(s)
  \/ DeviceLoss \/ Teardown \/ Reset

Spec == Init /\ [][Next]_vars

ProgressArrive == \E s \in Sources : Arrive(s)
ProgressAdoptRaw == \E s \in Sources : AdoptRaw(s)
ProgressPark == \E s \in Sources : Park(s)
ProgressAdmit == \E s \in Sources : Admit(s)
ProgressRetry == \E s \in Sources : Retry(s)
ProgressBuild == \E s \in Sources : Build(s)
ProgressReturnBorrow == \E s \in Sources : ReturnReplayBorrow(s)
ProgressPublish == \E s \in Sources : Publish(s)
ProgressBeginEncode == \E s \in Sources : BeginEncode(s)
ProgressJoin == \E s \in Sources : Join(s)
ProgressSubmit == \E s \in Sources : Submit(s)
ProgressCompleteGpu == \E s \in Sources : CompleteGpu(s)
ProgressFinishAdvance == \E s \in Sources : FinishAdvance(s)
ProgressReclaim == \E s \in Sources : Reclaim(s)

ProgressNext ==
  \/ ProgressArrive \/ ProgressAdoptRaw \/ ProgressPark \/ ProgressAdmit
  \/ ProgressRetry \/ ProgressBuild \/ ProgressReturnBorrow
  \/ ProgressPublish \/ ProgressBeginEncode \/ ProgressJoin
  \/ ProgressSubmit \/ ProgressCompleteGpu
  \/ ProgressFinishAdvance
  \/ ProgressReclaim

ProgressFairness ==
  /\ WF_vars(ProgressArrive)
  /\ WF_vars(ProgressAdoptRaw)
  /\ WF_vars(ProgressPark)
  /\ WF_vars(ProgressAdmit)
  /\ WF_vars(ProgressRetry)
  /\ WF_vars(ProgressBuild)
  /\ WF_vars(ProgressReturnBorrow)
  /\ WF_vars(ProgressPublish)
  /\ WF_vars(ProgressBeginEncode)
  /\ WF_vars(ProgressJoin)
  /\ WF_vars(ProgressSubmit)
  /\ WF_vars(ProgressCompleteGpu)
  /\ WF_vars(ProgressFinishAdvance)
  /\ WF_vars(ProgressReclaim)

ProgressSpec == Init /\ [][ProgressNext]_vars /\ ProgressFairness

TypeOK ==
  /\ phase \in [Sources -> Stages]
  /\ generation \in [Sources -> Nat]
  /\ epoch \in 1..2
  /\ nextArrival \in 1..(MaxSources + 1)
  /\ constructed \in [Sources -> 0..RequiredParts]
  /\ borrows \in [Sources -> 0..2]
  /\ childTotal \in [Sources -> 0..2]
  /\ joined \in [Sources -> 0..2]
  /\ authority \subseteq Sources
  /\ completed \subseteq Sources
  /\ noGpu \subseteq Sources
  /\ presentPending \subseteq Sources
  /\ presentSettled \subseteq Sources
  /\ occupancy \in 0..Capacity
  /\ waiting \subseteq Sources
  /\ observedWake \in [Sources -> Nat]
  /\ wake \in Nat
  /\ completedSeq \in 0..MaxSources
  /\ presentSeq \in 0..MaxSources
  /\ gpuCompletedTailSeq \in 0..MaxSources
  /\ finishWaterlineSeq \in 0..MaxSources
  /\ completedQueueDepth \in 0..Capacity
  /\ CompletionFrontierContiguous(finishWaterlineSeq,
      completedQueueDepth, gpuCompletedTailSeq)
  /\ stopped \in BOOLEAN
  /\ resetCount \in 0..1
  /\ owner \in [Sources -> STRING]
  /\ disposition \in [Sources -> STRING]

OccupancyExact == occupancy = Cardinality({s \in Sources : phase[s] \in OwnedStages})
GenerationQualified == \A s \in Sources : generation[s] <= epoch + 1
PublicationComplete ==
  \A s \in Sources : phase[s] \in {"FinalOwned", "Encoding", "GPUInFlight", "Completed"}
      => constructed[s] = RequiredParts /\ borrows[s] >= 0
JoinBeforeReceipt == \A s \in authority : joined[s] = childTotal[s] /\ borrows[s] = 0
CompletionPrefix == completed = 1..completedSeq
PresentSettledBeforeReclaim ==
  \A s \in PresentSources : phase[s] = "Reclaimed" => s \in presentSettled
NoGpuMilestone == \A s \in noGpu : phase[s] = "Reclaimed"
WakeOnReclaim ==
  \A s \in waiting : occupancy < Capacity /\ ~stopped
      => wake > observedWake[s]
ResetGenerationAdvances == resetCount > 0 => epoch > 1 /\
  \A s \in Sources : generation[s] > 1
OwnersAreExplicit ==
  \A s \in Sources : phase[s] # "Absent" =>
    owner[s] # "None" /\ disposition[s] # "None"

SelectedOwnerExact ==
  \A s \in Sources : phase[s] = "Encoding" =>
    IF s \in SelectedSources
    THEN owner[s] = "SelectedParallel"
    ELSE owner[s] = "SerialEncode"

ReceiptAuthorityExact ==
  \A s \in Sources : phase[s] = "GPUInFlight" => s \in authority

NormalCompletionState ==
  \A s \in Sources : owner[s] = "GpuCompletion" =>
    s \notin FailureSources /\
    phase[s] \in {"Completed", "Reclaimed"}

ParallelJoinState ==
  \A s \in Sources : s \in SelectedSources /\
      joined[s] = childTotal[s] /\ childTotal[s] > 0 =>
    s \in SelectedSources /\ phase[s] \in {"Encoding", "GPUInFlight",
      "Completed", "Reclaimed"}

NormalGpuCompletionPath ==
  <> (\E s \in Sources : s \notin FailureSources /\
    phase[s] = "Completed" /\
    owner[s] = "GpuCompletion")

NormalReclaimPath ==
  <> (\E s \in Sources : s \notin FailureSources /\
    phase[s] = "Reclaimed" /\
    owner[s] = "Reclaim")

ParallelJoinPath ==
  <> (\E s \in SelectedSources : joined[s] = childTotal[s] /\
    childTotal[s] > 0)

EventuallyReclaimed ==
  \A s \in Sources : phase[s] # "Absent" ~> phase[s] = "Reclaimed"

====
