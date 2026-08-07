---- MODULE SessionCapacityLease ----
(***************************************************************************
 * R-BACK-2.65 bounded capacity refinement. One generation-stamped lease
 * reserves a fixed physical-residency vector plus one complete ordinary
 * successor footprint. Synchronously encoded sources may retire out of that
 * vector while remaining charged to a separate bounded session-work list.
 * Completion is deliberately absent from grouping guards: it may reclaim
 * legacy submitted residency, but it cannot select a session boundary.
 *)

EXTENDS Naturals, Sequences, TLC

CONSTANTS MaxSources, MaxReady, HighWaterSources, HighWaterPages,
          MaxSessionSources, MaxSessionPages, OrdinaryMaxPages,
          SuccessorHeadroomSources, SuccessorHeadroomPages,
          MaxLeaseGeneration

SourceIds == 1 .. MaxSources
SourceIds0 == 0 .. MaxSources

PayloadPages(source) == IF source % 2 = 0 THEN OrdinaryMaxPages ELSE 1
ReservationPages(source) ==
  IF source % 2 = 0 THEN 2 * PayloadPages(source) - 1
  ELSE PayloadPages(source)
RECURSIVE SeqPages(_)
SeqPages(seq) == IF Len(seq) = 0 THEN 0
                 ELSE ReservationPages(Head(seq)) + SeqPages(Tail(seq))

VARIABLES nextSource, ready, writing, session, residentSession,
          submitted, submittedResident, completed, completedResident, reclaimed,
          leaseActive, leaseGeneration, capPending, submittedGroups,
          rollbackSources, boundaryCause, pressureWakeEpoch,
          capacityProgressGeneration, leaseWaitActive,
          leaseWaitObservedGeneration, startupPhase

vars == <<nextSource, ready, writing, session, residentSession,
          submitted, submittedResident, completed, completedResident, reclaimed,
          leaseActive, leaseGeneration, capPending, submittedGroups,
          rollbackSources, boundaryCause, pressureWakeEpoch,
          capacityProgressGeneration, leaseWaitActive,
          leaseWaitObservedGeneration, startupPhase>>

WritingResident == IF writing = 0 THEN <<>> ELSE <<writing>>
Resident == ready \o WritingResident \o residentSession \o
            submittedResident \o completedResident
SessionCanCharge(source) ==
  /\ Len(session) < MaxSessionSources
  /\ SeqPages(residentSession) + ReservationPages(source) <= MaxSessionPages

LeaseCapacityAvailable ==
  /\ Len(submittedResident \o completedResident) + MaxSessionSources +
       SuccessorHeadroomSources <= HighWaterSources
  /\ SeqPages(submittedResident \o completedResident) + MaxSessionPages +
       SuccessorHeadroomPages <= HighWaterPages

Init ==
  \* Preserve the original empty-start state space and add non-vacuous startup
  \* probes. In the denial probe, source 1 is a standalone submitted
  \* Clear+Present and source 2 is the following Ready Direct draw. With the
  \* production zero-slack lease policy, source 1 must reclaim before source 2
  \* can acquire its first session lease. In the Writing probe, two Ready
  \* sources plus their unique ordered-tail Writing successor fill physical
  \* capacity. The Writing claim is already covered by successor headroom, so
  \* it must not be counted again as older unavailable residency.
  /\ \/ /\ nextSource = 1
         /\ ready = <<>>
         /\ writing = 0
         /\ submitted = <<>>
         /\ startupPhase = "Inactive"
     \/ /\ nextSource = 3
         /\ ready = <<2>>
         /\ writing = 0
         /\ submitted = <<1>>
         /\ startupPhase = "NeedDenial"
     \/ /\ nextSource = 4
         /\ ready = <<1, 2>>
         /\ writing = 3
         /\ submitted = <<>>
         /\ startupPhase = "WritingNeedLease"
  /\ session = <<>>
  /\ residentSession = <<>>
  /\ submittedResident = submitted
  /\ completed = <<>>
  /\ completedResident = <<>>
  /\ reclaimed = 0
  /\ leaseActive = FALSE
  /\ leaseGeneration = 0
  /\ capPending = FALSE
  /\ submittedGroups = <<>>
  /\ rollbackSources = <<>>
  /\ boundaryCause = "None"
  /\ pressureWakeEpoch = 0
  /\ capacityProgressGeneration = 0
  /\ leaseWaitActive = FALSE
  /\ leaseWaitObservedGeneration = 0

PublishOrdinary ==
  /\ nextSource <= MaxSources
  /\ writing = 0
  /\ PayloadPages(nextSource) <= OrdinaryMaxPages
  /\ Len(Resident) < HighWaterSources
  /\ SeqPages(Resident) + ReservationPages(nextSource) <= HighWaterPages
  /\ Len(ready) < MaxReady
  /\ (~leaseActive \/ Len(ready) < SuccessorHeadroomSources)
  /\ ready' = Append(ready, nextSource)
  /\ nextSource' = nextSource + 1
  /\ UNCHANGED <<writing, session, residentSession, submitted, submittedResident,
                  completed, completedResident, reclaimed, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

BeginLeaseWait ==
  /\ startupPhase = "NeedDenial"
  /\ ~leaseActive
  /\ session = <<>>
  /\ ready # <<>>
  /\ ~LeaseCapacityAvailable
  /\ leaseWaitActive' = TRUE
  /\ leaseWaitObservedGeneration' = capacityProgressGeneration
  /\ startupPhase' = "Waiting"
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession,
                  submitted, submittedResident, completed, completedResident,
                  reclaimed, leaseActive, leaseGeneration, capPending,
                  submittedGroups, rollbackSources, boundaryCause,
                  pressureWakeEpoch, capacityProgressGeneration>>

AcquireLease ==
  /\ ~leaseActive
  /\ ~leaseWaitActive
  /\ startupPhase # "NeedDenial"
  /\ session = <<>>
  /\ ready # <<>>
  /\ PayloadPages(Head(ready)) <= OrdinaryMaxPages
  /\ leaseGeneration < MaxLeaseGeneration
  /\ LeaseCapacityAvailable
  /\ leaseActive' = TRUE
  /\ leaseGeneration' = leaseGeneration + 1
  /\ startupPhase' = IF startupPhase = "Woken"
                        THEN "Acquired"
                      ELSE IF startupPhase = "WritingNeedLease"
                        THEN "WritingLeaseAcquired" ELSE startupPhase
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession,
                  submitted, submittedResident, completed, completedResident,
                  reclaimed, capPending, submittedGroups, rollbackSources,
                  boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration>>

AdmitReadyHead ==
  /\ leaseActive
  /\ ~capPending
  /\ ready # <<>>
  /\ SessionCanCharge(Head(ready))
  /\ session' = Append(session, Head(ready))
  /\ residentSession' = Append(residentSession, Head(ready))
  /\ ready' = Tail(ready)
  /\ startupPhase' = IF startupPhase = "WritingLeaseAcquired"
                        THEN "WritingHeadAdmitted" ELSE startupPhase
  /\ UNCHANGED <<nextSource, writing, submitted, submittedResident, completed,
                  completedResident, reclaimed, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration>>

RetireEncodedHead ==
  /\ leaseActive
  /\ ~capPending
  /\ residentSession # <<>>
  /\ Head(residentSession) \in {session[i] : i \in DOMAIN session}
  /\ residentSession' = Tail(residentSession)
  /\ startupPhase' = IF startupPhase = "WritingHeadAdmitted"
                        THEN "WritingHeadRetired" ELSE startupPhase
  /\ UNCHANGED <<nextSource, ready, writing, session, submitted,
                  submittedResident, completed, completedResident, reclaimed,
                  leaseActive, leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration>>

PublishWritingSuccessor ==
  /\ writing # 0
  /\ leaseActive
  /\ startupPhase = "WritingHeadRetired"
  /\ Len(ready) < MaxReady
  /\ ready' = Append(ready, writing)
  /\ writing' = 0
  /\ startupPhase' = "WritingPublished"
  /\ UNCHANGED <<nextSource, session, residentSession, submitted,
                  submittedResident, completed, completedResident, reclaimed,
                  leaseActive, leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration>>

PostSessionCap ==
  /\ leaseActive
  /\ ~capPending
  /\ session # <<>>
  /\ ready # <<>>
  /\ ~SessionCanCharge(Head(ready))
  /\ capPending' = TRUE
  /\ boundaryCause' = "Cap"
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, submitted,
                  submittedResident, completed, completedResident,
                  reclaimed, leaseActive, leaseGeneration, submittedGroups,
                  rollbackSources, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

PostSemanticRelease ==
  /\ leaseActive
  /\ ~capPending
  /\ session # <<>>
  /\ capPending' = TRUE
  /\ boundaryCause' = "Semantic"
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, submitted,
                  submittedResident, completed, completedResident,
                  reclaimed, leaseActive, leaseGeneration, submittedGroups,
                  rollbackSources, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

PressureWake ==
  /\ pressureWakeEpoch < MaxSources
  /\ pressureWakeEpoch' = pressureWakeEpoch + 1
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, submitted,
                  submittedResident, completed, completedResident,
                  reclaimed, leaseActive, leaseGeneration, capPending,
                  submittedGroups, rollbackSources, boundaryCause,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

SubmitPredecessor ==
  /\ capPending
  /\ session # <<>>
  /\ submitted' = submitted \o session
  /\ submittedResident' = submittedResident \o residentSession
  /\ submittedGroups' = Append(submittedGroups, session)
  /\ session' = <<>>
  /\ residentSession' = <<>>
  /\ leaseActive' = FALSE
  /\ capPending' = FALSE
  /\ boundaryCause' = "None"
  /\ UNCHANGED <<nextSource, ready, writing, completed, completedResident, reclaimed,
                  leaseGeneration,
                  rollbackSources, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

CompleteSubmitted ==
  /\ submitted # <<>>
  /\ completed' = Append(completed, Head(submitted))
  /\ completedResident' =
       IF submittedResident # <<>> /\
          Head(submittedResident) = Head(submitted)
       THEN Append(completedResident, Head(submittedResident))
       ELSE completedResident
  /\ submittedResident' =
       IF submittedResident # <<>> /\
          Head(submittedResident) = Head(submitted)
       THEN Tail(submittedResident)
       ELSE submittedResident
  /\ submitted' = Tail(submitted)
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, reclaimed,
                  leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  capacityProgressGeneration, leaseWaitActive,
                  leaseWaitObservedGeneration, startupPhase>>

ReclaimCompleted ==
  /\ completed # <<>>
  /\ Head(completed) = reclaimed + 1
  /\ reclaimed' = Head(completed)
  /\ completed' = Tail(completed)
  /\ completedResident' =
       IF completedResident # <<>> /\
          Head(completedResident) = Head(completed)
       THEN Tail(completedResident)
       ELSE completedResident
  /\ capacityProgressGeneration' = capacityProgressGeneration + 1
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, submitted,
                  submittedResident, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch,
                  leaseWaitActive, leaseWaitObservedGeneration,
                  startupPhase>>

WakeLeaseWait ==
  /\ leaseWaitActive
  /\ capacityProgressGeneration # leaseWaitObservedGeneration
  /\ leaseWaitActive' = FALSE
  /\ startupPhase' = IF startupPhase = "Waiting"
                        THEN "Woken" ELSE startupPhase
  /\ UNCHANGED <<nextSource, ready, writing, session, residentSession, submitted,
                  submittedResident, completed, completedResident,
                  reclaimed, leaseActive, leaseGeneration, capPending,
                  submittedGroups, rollbackSources, boundaryCause,
                  pressureWakeEpoch, capacityProgressGeneration,
                  leaseWaitObservedGeneration>>

Next ==
  IF startupPhase = "NeedDenial"
  THEN BeginLeaseWait
  ELSE IF startupPhase = "WritingNeedLease"
  THEN AcquireLease
  ELSE IF startupPhase = "WritingLeaseAcquired"
  THEN AdmitReadyHead
  ELSE IF startupPhase = "WritingHeadAdmitted"
  THEN RetireEncodedHead
  ELSE IF startupPhase = "WritingHeadRetired"
  THEN PublishWritingSuccessor
  ELSE \/ PublishOrdinary
       \/ AcquireLease
       \/ AdmitReadyHead
       \/ RetireEncodedHead
       \/ PublishWritingSuccessor
       \/ PostSessionCap
       \/ PostSemanticRelease
       \/ PressureWake
       \/ SubmitPredecessor
       \/ CompleteSubmitted
       \/ ReclaimCompleted
       \/ WakeLeaseWait

Spec == Init /\ [][Next]_vars
  /\ WF_vars(AcquireLease)
  /\ WF_vars(AdmitReadyHead)
  /\ WF_vars(RetireEncodedHead)
  /\ WF_vars(PublishWritingSuccessor)
  /\ WF_vars(PostSessionCap)
  /\ WF_vars(SubmitPredecessor)
  /\ WF_vars(CompleteSubmitted)
  /\ WF_vars(ReclaimCompleted)
  /\ WF_vars(BeginLeaseWait)
  /\ WF_vars(WakeLeaseWait)

TypeOK ==
  /\ nextSource \in 1 .. (MaxSources + 1)
  /\ ready \in Seq(SourceIds)
  /\ writing \in SourceIds0
  /\ session \in Seq(SourceIds)
  /\ residentSession \in Seq(SourceIds)
  /\ submitted \in Seq(SourceIds)
  /\ submittedResident \in Seq(SourceIds)
  /\ completed \in Seq(SourceIds)
  /\ completedResident \in Seq(SourceIds)
  /\ reclaimed \in SourceIds0
  /\ leaseActive \in BOOLEAN
  /\ leaseGeneration \in 0 .. MaxLeaseGeneration
  /\ capPending \in BOOLEAN
  /\ submittedGroups \in Seq(Seq(SourceIds))
  /\ rollbackSources \in Seq(SourceIds)
  /\ boundaryCause \in {"None", "Cap", "Semantic", "Pressure"}
  /\ pressureWakeEpoch \in 0 .. MaxSources
  /\ capacityProgressGeneration \in 0 .. MaxSources
  /\ leaseWaitActive \in BOOLEAN
  /\ leaseWaitObservedGeneration \in 0 .. MaxSources
  /\ startupPhase \in
       {"Inactive", "NeedDenial", "Waiting", "Woken", "Acquired",
        "WritingNeedLease", "WritingLeaseAcquired", "WritingHeadAdmitted",
        "WritingHeadRetired", "WritingPublished"}

BoundedCapacity ==
  /\ Len(ready) <= MaxReady
  /\ Len(Resident) <= HighWaterSources
  /\ SeqPages(Resident) <= HighWaterPages
  /\ Len(session) <= MaxSessionSources
  /\ Len(residentSession) <= Len(session)
  /\ SeqPages(residentSession) <= MaxSessionPages

LeaseOwnsCompleteHeadroom ==
  /\ MaxSessionSources + SuccessorHeadroomSources <= HighWaterSources
  /\ MaxSessionPages + SuccessorHeadroomPages <= HighWaterPages
  /\ SuccessorHeadroomSources >= 1
  /\ SuccessorHeadroomPages >= 2 * OrdinaryMaxPages - 1
  /\ (leaseActive => leaseGeneration > 0)
  /\ (leaseActive =>
        Len(ready) + (IF writing = 0 THEN 0 ELSE 1)
          <= MaxSessionSources + SuccessorHeadroomSources)
  /\ (leaseActive =>
        SeqPages(ready \o WritingResident)
          <= MaxSessionPages + SuccessorHeadroomPages)

WritingSuccessorIsUnique ==
  /\ (writing = 0 \/ writing \in SourceIds)
  /\ (writing # 0 =>
        writing \notin {ready[i] : i \in DOMAIN ready})
  /\ (writing # 0 =>
        writing \notin {session[i] : i \in DOMAIN session})
  /\ (startupPhase \in
        {"WritingNeedLease", "WritingLeaseAcquired", "WritingHeadAdmitted",
         "WritingHeadRetired"} => writing # 0)
  /\ (startupPhase = "WritingPublished" => writing = 0)

CapCandidateStaysReady ==
  capPending /\ boundaryCause = "Cap" /\ ready # <<>> /\
    ~SessionCanCharge(Head(ready))
    => Head(ready) \notin {session[i] : i \in DOMAIN session}

NoPressureCreatedRelease ==
  /\ (capPending <=> boundaryCause # "None")
  /\ boundaryCause # "Pressure"

CapacityWakeMatchesProgress ==
  /\ leaseWaitObservedGeneration <= capacityProgressGeneration
  /\ (startupPhase = "Waiting" => leaseWaitActive)
  /\ (startupPhase \in {"Woken", "Acquired"} => ~leaseWaitActive)

SubmittedGroupsRespectCap ==
  \A group \in {submittedGroups[i] : i \in DOMAIN submittedGroups} :
    Len(group) <= MaxSessionSources

ResidencyIsSeparateFromWork ==
  /\ Len(residentSession) <= Len(session)
  /\ \A s \in {residentSession[i] : i \in DOMAIN residentSession} :
       s \in {session[i] : i \in DOMAIN session}

Safety ==
  /\ TypeOK
  /\ BoundedCapacity
  /\ LeaseOwnsCompleteHeadroom
  /\ WritingSuccessorIsUnique
  /\ CapCandidateStaysReady
  /\ NoPressureCreatedRelease
  /\ CapacityWakeMatchesProgress
  /\ SubmittedGroupsRespectCap
  /\ ResidencyIsSeparateFromWork

CapProgress == capPending ~> ~capPending

StartupCapacityWakeProgress ==
  (startupPhase = "Waiting") ~> (startupPhase \in {"Woken", "Acquired"})

StartupDirectLeaseProgress ==
  (startupPhase = "Woken") ~> (startupPhase = "Acquired")

WritingSuccessorStartupProgress ==
  (startupPhase = "WritingNeedLease") ~>
    (startupPhase = "WritingPublished")

====
