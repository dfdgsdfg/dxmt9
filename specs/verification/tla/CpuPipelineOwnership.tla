---- MODULE CpuPipelineOwnership ----
(*
 * R-BACK-2.88 / R-VERIF-2.23 bounded end-to-end CPU pipeline refinement.
 *
 * One phase per source deliberately composes the existing admission, wake,
 * replay-borrow, publication, encode-join, completion, Present, failure, and
 * shutdown interfaces without taking the product of their component states.
 * Source 2 in the checked configuration is a Present-only source.  With one
 * queue credit it must park behind source 1, consume the reclaim wake, and
 * still advance both completion waterlines.
 *
 * The four Discipline constants are mutation switches used only by the
 * executable expected-failure configurations.  The production configuration
 * requires an admission wake, complete publication, join before completion
 * authority, and completion before owner reclaim.
 *)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS MaxSources, Capacity, RequiredParts, PresentSources, ParallelSources,
          FailureSources, StateOnlySources, AllowShutdown,
          WakeDiscipline, PublicationDiscipline, JoinDiscipline,
          ReclaimDiscipline

Sources == 1 .. MaxSources
Phases == {"Absent", "ProducerOwned", "RawOwned", "ReplayBorrowed",
           "FinalOwned", "Encoding", "GPUInFlight", "Reclaimed"}
OwnedPhases == {"ReplayBorrowed", "FinalOwned", "Encoding", "GPUInFlight"}

VARIABLES
  phase,
  nextArrival,
  constructed,
  borrowCount,
  joinedChildren,
  published,
  fullyBuilt,
  joined,
  completionAuthority,
  completed,
  noGpuTerminal,
  failedOnce,
  occupancy,
  admissionWaiting,
  observedWakeGeneration,
  wakeGeneration,
  completedSeq,
  presentSeq,
  stopped

vars ==
  <<phase, nextArrival, constructed, borrowCount, joinedChildren,
    published, fullyBuilt, joined, completionAuthority, completed,
    noGpuTerminal, failedOnce, occupancy, admissionWaiting,
    observedWakeGeneration, wakeGeneration, completedSeq, presentSeq,
    stopped>>

ChildCount(s) == IF s \in ParallelSources THEN 2 ELSE 1
Owned(s) == phase[s] \in OwnedPhases
Terminal(s) == phase[s] = "Reclaimed"
RawSources == {s \in Sources : phase[s] = "RawOwned"}
ProducerSources == {s \in Sources : phase[s] = "ProducerOwned"}
Oldest(set) == CHOOSE s \in set : \A t \in set : s <= t

Init ==
  /\ phase = [s \in Sources |-> "Absent"]
  /\ nextArrival = 1
  /\ constructed = [s \in Sources |-> 0]
  /\ borrowCount = [s \in Sources |-> 0]
  /\ joinedChildren = [s \in Sources |-> 0]
  /\ published = {}
  /\ fullyBuilt = {}
  /\ joined = {}
  /\ completionAuthority = {}
  /\ completed = {}
  /\ noGpuTerminal = {}
  /\ failedOnce = {}
  /\ occupancy = 0
  /\ admissionWaiting = {}
  /\ observedWakeGeneration = [s \in Sources |-> 0]
  /\ wakeGeneration = 0
  /\ completedSeq = 0
  /\ presentSeq = 0
  /\ stopped = FALSE

Arrive ==
  /\ ~stopped
  /\ nextArrival \in Sources
  /\ phase' = [phase EXCEPT ![nextArrival] = "ProducerOwned"]
  /\ nextArrival' = nextArrival + 1
  /\ UNCHANGED <<constructed, borrowCount, joinedChildren, published,
                  fullyBuilt, joined, completionAuthority, completed,
                  noGpuTerminal, failedOnce, occupancy, admissionWaiting,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

AdoptRaw(s) ==
  /\ ~stopped
  /\ phase[s] = "ProducerOwned"
  /\ s = Oldest(ProducerSources)
  /\ phase' = [phase EXCEPT ![s] = "RawOwned"]
  /\ UNCHANGED <<nextArrival, constructed, borrowCount, joinedChildren,
                  published, fullyBuilt, joined, completionAuthority,
                  completed, noGpuTerminal, failedOnce, occupancy,
                  admissionWaiting, observedWakeGeneration, wakeGeneration,
                  completedSeq, presentSeq, stopped>>

BeginReplay(s) ==
  /\ ~stopped
  /\ phase[s] = "RawOwned"
  /\ s = Oldest(RawSources)
  /\ s \notin admissionWaiting
  /\ occupancy < Capacity
  /\ phase' = [phase EXCEPT ![s] = "ReplayBorrowed"]
  /\ constructed' = [constructed EXCEPT ![s] = 0]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = 1]
  /\ occupancy' = occupancy + 1
  /\ UNCHANGED <<nextArrival, joinedChildren, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  admissionWaiting, observedWakeGeneration, wakeGeneration,
                  completedSeq, presentSeq, stopped>>

ParkAdmission(s) ==
  /\ ~stopped
  /\ phase[s] = "RawOwned"
  /\ s = Oldest(RawSources)
  /\ occupancy = Capacity
  /\ s \notin admissionWaiting
  /\ admissionWaiting' = admissionWaiting \cup {s}
  /\ observedWakeGeneration' =
       [observedWakeGeneration EXCEPT ![s] = wakeGeneration]
  /\ UNCHANGED <<phase, nextArrival, constructed, borrowCount,
                  joinedChildren, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  occupancy, wakeGeneration, completedSeq, presentSeq,
                  stopped>>

RetryAdmission(s) ==
  /\ ~stopped
  /\ phase[s] = "RawOwned"
  /\ s = Oldest(RawSources)
  /\ s \in admissionWaiting
  /\ occupancy < Capacity
  /\ wakeGeneration > observedWakeGeneration[s]
  /\ phase' = [phase EXCEPT ![s] = "ReplayBorrowed"]
  /\ constructed' = [constructed EXCEPT ![s] = 0]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = 1]
  /\ occupancy' = occupancy + 1
  /\ admissionWaiting' = admissionWaiting \ {s}
  /\ UNCHANGED <<nextArrival, joinedChildren, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

BuildPart(s) ==
  /\ ~stopped
  /\ phase[s] = "ReplayBorrowed"
  /\ constructed[s] < RequiredParts
  /\ constructed' = [constructed EXCEPT ![s] = @ + 1]
  /\ UNCHANGED <<phase, nextArrival, borrowCount, joinedChildren, published,
                  fullyBuilt, joined, completionAuthority, completed,
                  noGpuTerminal, failedOnce, occupancy, admissionWaiting,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

PublishFinal(s) ==
  /\ ~stopped
  /\ phase[s] = "ReplayBorrowed"
  /\ s \notin FailureSources \ failedOnce
  /\ IF PublicationDiscipline = "Complete"
       THEN constructed[s] = RequiredParts
       ELSE constructed[s] > 0
  /\ phase' = [phase EXCEPT ![s] = "FinalOwned"]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = 0]
  /\ published' = published \cup {s}
  /\ fullyBuilt' = IF constructed[s] = RequiredParts
       THEN fullyBuilt \cup {s}
       ELSE fullyBuilt
  /\ UNCHANGED <<nextArrival, constructed, joinedChildren, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  occupancy, admissionWaiting, observedWakeGeneration,
                  wakeGeneration, completedSeq, presentSeq, stopped>>

BeginEncoding(s) ==
  /\ ~stopped
  /\ phase[s] = "FinalOwned"
  /\ phase' = [phase EXCEPT ![s] = "Encoding"]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = ChildCount(s)]
  /\ joinedChildren' = [joinedChildren EXCEPT ![s] = 0]
  /\ UNCHANGED <<nextArrival, constructed, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  occupancy, admissionWaiting, observedWakeGeneration,
                  wakeGeneration, completedSeq, presentSeq, stopped>>

FinishChild(s) ==
  /\ ~stopped
  /\ phase[s] = "Encoding"
  /\ borrowCount[s] > 0
  /\ borrowCount' = [borrowCount EXCEPT ![s] = @ - 1]
  /\ joinedChildren' = [joinedChildren EXCEPT ![s] = @ + 1]
  /\ joined' = IF joinedChildren[s] + 1 = ChildCount(s)
       THEN joined \cup {s}
       ELSE joined
  /\ UNCHANGED <<phase, nextArrival, constructed, published, fullyBuilt,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  occupancy, admissionWaiting, observedWakeGeneration,
                  wakeGeneration, completedSeq, presentSeq, stopped>>

SubmitGpu(s) ==
  /\ ~stopped
  /\ phase[s] = "Encoding"
  /\ IF JoinDiscipline = "AfterJoin"
       THEN /\ borrowCount[s] = 0
            /\ s \in joined
       ELSE TRUE
  /\ phase' = [phase EXCEPT ![s] = "GPUInFlight"]
  /\ completionAuthority' = completionAuthority \cup {s}
  /\ UNCHANGED <<nextArrival, constructed, borrowCount, joinedChildren,
                  published, fullyBuilt, joined, completed, noGpuTerminal,
                  failedOnce, occupancy, admissionWaiting,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

CompleteGpu(s) ==
  /\ phase[s] = "GPUInFlight"
  /\ s \in completionAuthority
  /\ completedSeq = s - 1
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ completed' = completed \cup {s}
  /\ completedSeq' = s
  /\ presentSeq' = IF s \in PresentSources THEN s ELSE presentSeq
  /\ occupancy' = occupancy - 1
  /\ wakeGeneration' = IF admissionWaiting # {} /\
                              WakeDiscipline = "Notify"
       THEN wakeGeneration + 1
       ELSE wakeGeneration
  /\ UNCHANGED <<nextArrival, constructed, borrowCount, joinedChildren,
                  published, fullyBuilt, joined, completionAuthority,
                  noGpuTerminal, failedOnce, admissionWaiting,
                  observedWakeGeneration, stopped>>

RollbackReplay(s) ==
  /\ ~stopped
  /\ s \in FailureSources \ failedOnce
  /\ phase[s] = "ReplayBorrowed"
  /\ phase' = [phase EXCEPT ![s] = "RawOwned"]
  /\ constructed' = [constructed EXCEPT ![s] = 0]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = 0]
  /\ failedOnce' = failedOnce \cup {s}
  /\ occupancy' = occupancy - 1
  /\ wakeGeneration' = IF admissionWaiting # {} /\
                              WakeDiscipline = "Notify"
       THEN wakeGeneration + 1
       ELSE wakeGeneration
  /\ UNCHANGED <<nextArrival, joinedChildren, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal,
                  admissionWaiting, observedWakeGeneration, completedSeq,
                  presentSeq, stopped>>

FinishStateOnly(s) ==
  /\ ~stopped
  /\ s \in StateOnlySources
  /\ phase[s] = "RawOwned"
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ noGpuTerminal' = noGpuTerminal \cup {s}
  /\ UNCHANGED <<nextArrival, constructed, borrowCount, joinedChildren,
                  published, fullyBuilt, joined, completionAuthority,
                  completed, failedOnce, occupancy, admissionWaiting,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

PrematureReclaim(s) ==
  /\ ReclaimDiscipline = "Premature"
  /\ Owned(s)
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ occupancy' = occupancy - 1
  /\ UNCHANGED <<nextArrival, constructed, borrowCount, joinedChildren,
                  published, fullyBuilt, joined, completionAuthority,
                  completed, noGpuTerminal, failedOnce, admissionWaiting,
                  observedWakeGeneration, wakeGeneration, completedSeq,
                  presentSeq, stopped>>

RequestShutdown ==
  /\ AllowShutdown
  /\ ~stopped
  /\ stopped' = TRUE
  /\ wakeGeneration' = IF admissionWaiting # {} /\
                              WakeDiscipline = "Notify"
       THEN wakeGeneration + 1
       ELSE wakeGeneration
  /\ UNCHANGED <<phase, nextArrival, constructed, borrowCount,
                  joinedChildren, published, fullyBuilt, joined,
                  completionAuthority, completed, noGpuTerminal, failedOnce,
                  occupancy, admissionWaiting, observedWakeGeneration,
                  completedSeq, presentSeq>>

DrainShutdown(s) ==
  /\ stopped
  /\ phase[s] # "Absent"
  /\ phase[s] # "Reclaimed"
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ borrowCount' = [borrowCount EXCEPT ![s] = 0]
  /\ occupancy' = IF Owned(s) THEN occupancy - 1 ELSE occupancy
  /\ admissionWaiting' = admissionWaiting \ {s}
  /\ noGpuTerminal' = noGpuTerminal \cup {s}
  /\ UNCHANGED <<nextArrival, constructed, joinedChildren, published,
                  fullyBuilt, joined, completionAuthority, completed,
                  failedOnce, observedWakeGeneration, wakeGeneration,
                  completedSeq, presentSeq, stopped>>

Next ==
  \/ Arrive
  \/ \E s \in Sources : AdoptRaw(s)
  \/ \E s \in Sources : BeginReplay(s)
  \/ \E s \in Sources : ParkAdmission(s)
  \/ \E s \in Sources : RetryAdmission(s)
  \/ \E s \in Sources : BuildPart(s)
  \/ \E s \in Sources : PublishFinal(s)
  \/ \E s \in Sources : BeginEncoding(s)
  \/ \E s \in Sources : FinishChild(s)
  \/ \E s \in Sources : SubmitGpu(s)
  \/ \E s \in Sources : CompleteGpu(s)
  \/ \E s \in Sources : RollbackReplay(s)
  \/ \E s \in Sources : FinishStateOnly(s)
  \/ \E s \in Sources : PrematureReclaim(s)
  \/ RequestShutdown
  \/ \E s \in Sources : DrainShutdown(s)

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(Arrive)
  /\ \A s \in Sources :
       /\ WF_vars(AdoptRaw(s))
       /\ WF_vars(BeginReplay(s))
       /\ WF_vars(ParkAdmission(s))
       /\ WF_vars(RetryAdmission(s))
       /\ WF_vars(BuildPart(s))
       /\ WF_vars(PublishFinal(s))
       /\ WF_vars(BeginEncoding(s))
       /\ WF_vars(FinishChild(s))
       /\ WF_vars(SubmitGpu(s))
       /\ WF_vars(CompleteGpu(s))
       /\ WF_vars(RollbackReplay(s))
       /\ WF_vars(FinishStateOnly(s))
       /\ WF_vars(DrainShutdown(s))

TypeOK ==
  /\ phase \in [Sources -> Phases]
  /\ nextArrival \in 1 .. (MaxSources + 1)
  /\ constructed \in [Sources -> 0 .. RequiredParts]
  /\ borrowCount \in [Sources -> 0 .. 2]
  /\ joinedChildren \in [Sources -> 0 .. 2]
  /\ published \subseteq Sources
  /\ fullyBuilt \subseteq Sources
  /\ joined \subseteq Sources
  /\ completionAuthority \subseteq Sources
  /\ completed \subseteq Sources
  /\ noGpuTerminal \subseteq Sources
  /\ failedOnce \subseteq Sources
  /\ occupancy \in 0 .. Capacity
  /\ admissionWaiting \subseteq Sources
  /\ observedWakeGeneration \in [Sources -> Nat]
  /\ wakeGeneration \in Nat
  /\ completedSeq \in 0 .. MaxSources
  /\ presentSeq \in 0 .. MaxSources
  /\ stopped \in BOOLEAN

OccupancyExact ==
  occupancy = Cardinality({s \in Sources : Owned(s)})

PublicationIsComplete == published \subseteq fullyBuilt

BorrowIsSynchronous ==
  /\ \A s \in Sources : phase[s] = "ReplayBorrowed" => borrowCount[s] = 1
  /\ \A s \in Sources : phase[s] \notin {"ReplayBorrowed", "Encoding"}
       => borrowCount[s] = 0

CompletionAuthorityAfterJoin == completionAuthority \subseteq joined

NoPrematureReclaim ==
  {s \in Sources : Terminal(s)} \subseteq completed \cup noGpuTerminal

CompletedPrefix == completed = 1 .. completedSeq

PresentCompletionOrdered ==
  /\ presentSeq <= completedSeq
  /\ \A s \in PresentSources \cap completed : s <= presentSeq

AdmissionReleaseNotifies ==
  \A s \in admissionWaiting :
    occupancy < Capacity /\ ~stopped
      => wakeGeneration > observedWakeGeneration[s]

EverySourceEventuallyReclaimed ==
  \A s \in Sources : phase[s] # "Absent" ~> phase[s] = "Reclaimed"

PresentOnlySourceEventuallyCompletes ==
  [](~stopped) => <> (2 \in completed /\ presentSeq = 2)

AdmissionWaitEventuallyRetries ==
  \A s \in Sources : s \in admissionWaiting ~> phase[s] # "RawOwned"

====
