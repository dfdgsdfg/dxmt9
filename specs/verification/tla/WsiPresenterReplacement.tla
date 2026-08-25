---------------------- MODULE WsiPresenterReplacement ----------------------
EXTENDS Naturals, TLC

CONSTANTS MaxUsers, MaxPresents

VARIABLES phase, gate, users, arena, pendingOldUses,
          oldRegistered, candidateRegistered, oldReleased,
          attemptedPresents, acceptedPresents, completedPresents,
          waitingPresents, pendingPresents

vars == <<phase, gate, users, arena, pendingOldUses,
          oldRegistered, candidateRegistered, oldReleased,
          attemptedPresents, acceptedPresents, completedPresents,
          waitingPresents, pendingPresents>>

Init ==
  /\ phase = "Idle"
  /\ gate = FALSE
  /\ users = 0
  /\ arena = FALSE
  /\ pendingOldUses = 0
  /\ oldRegistered = TRUE
  /\ candidateRegistered = FALSE
  /\ oldReleased = FALSE
  /\ attemptedPresents = 0
  /\ acceptedPresents = 0
  /\ completedPresents = 0
  /\ waitingPresents = 0
  /\ pendingPresents = 0

StartCandidate ==
  /\ phase = "Idle"
  /\ phase' = "Candidate"
  /\ candidateRegistered' = TRUE
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  oldRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

CandidateFailure ==
  /\ phase = "Candidate"
  /\ phase' = "Failed"
  /\ candidateRegistered' = FALSE
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  oldRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

PresentAttempt ==
  /\ attemptedPresents < MaxPresents
  /\ attemptedPresents' = attemptedPresents + 1
  /\ waitingPresents' = waitingPresents + 1
  /\ UNCHANGED <<phase, gate, users, arena, pendingOldUses,
                  oldRegistered, candidateRegistered, oldReleased,
                  acceptedPresents, completedPresents, pendingPresents>>

PresentEnter ==
  /\ ~gate
  /\ waitingPresents > 0
  /\ users < MaxUsers
  /\ users' = users + 1
  /\ waitingPresents' = waitingPresents - 1
  /\ acceptedPresents' = acceptedPresents + 1
  /\ UNCHANGED <<phase, gate, arena, pendingOldUses,
                  oldRegistered, candidateRegistered, oldReleased,
                  attemptedPresents, completedPresents, pendingPresents>>

PresentPublish ==
  /\ users > 0
  /\ pendingOldUses < MaxUsers
  /\ users' = users - 1
  /\ pendingPresents' = pendingPresents + 1
  /\ pendingOldUses' = IF oldRegistered
                        THEN pendingOldUses + 1
                        ELSE pendingOldUses
  /\ UNCHANGED <<phase, gate, arena, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents>>

ArenaBegin ==
  /\ ~gate
  /\ ~oldReleased
  /\ ~arena
  /\ arena' = TRUE
  /\ UNCHANGED <<phase, gate, users, pendingOldUses,
                  oldRegistered, candidateRegistered, oldReleased,
                  attemptedPresents, acceptedPresents, completedPresents,
                  waitingPresents, pendingPresents>>

ArenaPublish ==
  /\ arena
  /\ pendingOldUses < MaxUsers
  /\ arena' = FALSE
  /\ pendingOldUses' = pendingOldUses + 1
  /\ UNCHANGED <<phase, gate, users, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

BeginQuiescence ==
  /\ phase = "Candidate"
  /\ ~gate
  /\ ~arena
  /\ gate' = TRUE
  /\ phase' = "Quiescing"
  /\ UNCHANGED <<users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

GpuComplete ==
  /\ pendingPresents > 0
  /\ pendingPresents' = pendingPresents - 1
  /\ completedPresents' = completedPresents + 1
  /\ pendingOldUses' = IF pendingOldUses > 0
                        THEN pendingOldUses - 1
                        ELSE pendingOldUses
  /\ UNCHANGED <<phase, gate, users, arena, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, waitingPresents>>

GpuCompleteArena ==
  /\ pendingOldUses > pendingPresents
  /\ pendingOldUses' = pendingOldUses - 1
  /\ UNCHANGED <<phase, gate, users, arena, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

FenceComplete ==
  /\ phase = "Quiescing"
  /\ users = 0
  /\ ~arena
  /\ pendingOldUses = 0
  /\ phase' = "Quiescent"
  /\ UNCHANGED <<gate, users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

CommitReplacement ==
  /\ phase = "Quiescent"
  /\ candidateRegistered
  /\ oldRegistered' = FALSE
  /\ phase' = "Swapped"
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  candidateRegistered, oldReleased, attemptedPresents,
                  acceptedPresents, completedPresents, waitingPresents,
                  pendingPresents>>

ReleaseOld ==
  /\ phase = "Swapped"
  /\ gate
  /\ phase' = "Released"
  /\ gate' = FALSE
  /\ oldReleased' = TRUE
  /\ UNCHANGED <<users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered, attemptedPresents, acceptedPresents,
                  completedPresents, waitingPresents, pendingPresents>>

Stutter ==
  UNCHANGED vars

Next ==
  \/ StartCandidate
  \/ CandidateFailure
  \/ PresentAttempt
  \/ PresentEnter
  \/ PresentPublish
  \/ ArenaBegin
  \/ ArenaPublish
  \/ BeginQuiescence
  \/ GpuComplete
  \/ GpuCompleteArena
  \/ FenceComplete
  \/ CommitReplacement
  \/ ReleaseOld
  \/ Stutter

TypeOK ==
  /\ phase \in {"Idle", "Candidate", "Failed", "Quiescing",
                 "Quiescent", "Swapped", "Released"}
  /\ gate \in BOOLEAN
  /\ users \in 0..MaxUsers
  /\ arena \in BOOLEAN
  /\ pendingOldUses \in 0..MaxUsers
  /\ oldRegistered \in BOOLEAN
  /\ candidateRegistered \in BOOLEAN
  /\ oldReleased \in BOOLEAN
  /\ attemptedPresents \in 0..MaxPresents
  /\ acceptedPresents \in 0..MaxPresents
  /\ completedPresents \in 0..MaxPresents
  /\ waitingPresents \in 0..MaxPresents
  /\ pendingPresents \in 0..MaxPresents

PresentOrdinalConservation ==
  /\ attemptedPresents = waitingPresents + users +
       pendingPresents + completedPresents
  /\ acceptedPresents = users + pendingPresents + completedPresents
  /\ completedPresents <= acceptedPresents
  /\ acceptedPresents <= attemptedPresents

CandidateFailurePreservesOld ==
  phase = "Failed" => oldRegistered /\ ~oldReleased

ReleaseRequiresActualQuiescence ==
  oldReleased =>
    ~oldRegistered /\ pendingOldUses = 0

CommittedReplacementWasQuiescent ==
  phase \in {"Quiescent", "Swapped"} =>
    users = 0 /\ ~arena /\ pendingOldUses = 0

GateHeldThroughRegistrySwap ==
  phase \in {"Quiescing", "Quiescent", "Swapped"} => gate

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(PresentAttempt)
  /\ SF_vars(PresentEnter)
  /\ SF_vars(PresentPublish)
  /\ SF_vars(GpuComplete)
  /\ WF_vars(GpuCompleteArena)
  /\ WF_vars(BeginQuiescence)
  /\ WF_vars(FenceComplete)
  /\ WF_vars(CommitReplacement)
  /\ WF_vars(ReleaseOld)

AttemptedPresentsEventuallyComplete ==
  attemptedPresents = MaxPresents ~> completedPresents = MaxPresents

=============================================================================
