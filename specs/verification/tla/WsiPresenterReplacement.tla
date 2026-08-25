---------------------- MODULE WsiPresenterReplacement ----------------------
EXTENDS Naturals, TLC

CONSTANT MaxUsers

VARIABLES phase, gate, users, arena, pendingOldUses,
          oldRegistered, candidateRegistered, oldReleased

vars == <<phase, gate, users, arena, pendingOldUses,
          oldRegistered, candidateRegistered, oldReleased>>

Init ==
  /\ phase = "Idle"
  /\ gate = FALSE
  /\ users = 0
  /\ arena = FALSE
  /\ pendingOldUses = 0
  /\ oldRegistered = TRUE
  /\ candidateRegistered = FALSE
  /\ oldReleased = FALSE

StartCandidate ==
  /\ phase = "Idle"
  /\ phase' = "Candidate"
  /\ candidateRegistered' = TRUE
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  oldRegistered, oldReleased>>

CandidateFailure ==
  /\ phase = "Candidate"
  /\ phase' = "Failed"
  /\ candidateRegistered' = FALSE
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  oldRegistered, oldReleased>>

PresentEnter ==
  /\ ~gate
  /\ ~oldReleased
  /\ users < MaxUsers
  /\ users' = users + 1
  /\ UNCHANGED <<phase, gate, arena, pendingOldUses,
                  oldRegistered, candidateRegistered, oldReleased>>

PresentPublish ==
  /\ users > 0
  /\ pendingOldUses < MaxUsers
  /\ users' = users - 1
  /\ pendingOldUses' = pendingOldUses + 1
  /\ UNCHANGED <<phase, gate, arena, oldRegistered,
                  candidateRegistered, oldReleased>>

ArenaBegin ==
  /\ ~gate
  /\ ~oldReleased
  /\ ~arena
  /\ arena' = TRUE
  /\ UNCHANGED <<phase, gate, users, pendingOldUses,
                  oldRegistered, candidateRegistered, oldReleased>>

ArenaPublish ==
  /\ arena
  /\ pendingOldUses < MaxUsers
  /\ arena' = FALSE
  /\ pendingOldUses' = pendingOldUses + 1
  /\ UNCHANGED <<phase, gate, users, oldRegistered,
                  candidateRegistered, oldReleased>>

BeginQuiescence ==
  /\ phase = "Candidate"
  /\ ~gate
  /\ ~arena
  /\ gate' = TRUE
  /\ phase' = "Quiescing"
  /\ UNCHANGED <<users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered, oldReleased>>

GpuComplete ==
  /\ pendingOldUses > 0
  /\ pendingOldUses' = pendingOldUses - 1
  /\ UNCHANGED <<phase, gate, users, arena, oldRegistered,
                  candidateRegistered, oldReleased>>

FenceComplete ==
  /\ phase = "Quiescing"
  /\ users = 0
  /\ ~arena
  /\ pendingOldUses = 0
  /\ phase' = "Quiescent"
  /\ UNCHANGED <<gate, users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered, oldReleased>>

CommitReplacement ==
  /\ phase = "Quiescent"
  /\ candidateRegistered
  /\ oldRegistered' = FALSE
  /\ phase' = "Swapped"
  /\ UNCHANGED <<gate, users, arena, pendingOldUses,
                  candidateRegistered, oldReleased>>

ReleaseOld ==
  /\ phase = "Swapped"
  /\ gate
  /\ phase' = "Released"
  /\ gate' = FALSE
  /\ oldReleased' = TRUE
  /\ UNCHANGED <<users, arena, pendingOldUses, oldRegistered,
                  candidateRegistered>>

Stutter ==
  UNCHANGED vars

Next ==
  \/ StartCandidate
  \/ CandidateFailure
  \/ PresentEnter
  \/ PresentPublish
  \/ ArenaBegin
  \/ ArenaPublish
  \/ BeginQuiescence
  \/ GpuComplete
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

CandidateFailurePreservesOld ==
  phase = "Failed" => oldRegistered /\ ~oldReleased

ReleaseRequiresActualQuiescence ==
  oldReleased =>
    ~oldRegistered /\ users = 0 /\ ~arena /\ pendingOldUses = 0

CommittedReplacementWasQuiescent ==
  phase \in {"Quiescent", "Swapped", "Released"} =>
    users = 0 /\ ~arena /\ pendingOldUses = 0

GateHeldThroughRegistrySwap ==
  phase \in {"Quiescing", "Quiescent", "Swapped"} => gate

=============================================================================
