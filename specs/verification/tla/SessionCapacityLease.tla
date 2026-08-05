---- MODULE SessionCapacityLease ----
(***************************************************************************
 * R-BACK-2.65 bounded capacity refinement. One generation-stamped lease
 * reserves a fixed unsubmitted-session vector plus one complete ordinary
 * successor footprint. Completion is deliberately absent from admission and
 * grouping guards: it may reclaim submitted residency, but it cannot select a
 * session boundary. An over-cap Ready head stays in Ready until the exact
 * predecessor group is submitted.
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

VARIABLES nextSource, ready, session, submitted, completed, reclaimed,
          leaseActive, leaseGeneration, capPending, submittedGroups,
          rollbackSources, boundaryCause, pressureWakeEpoch

vars == <<nextSource, ready, session, submitted, completed, reclaimed,
          leaseActive, leaseGeneration, capPending, submittedGroups,
          rollbackSources, boundaryCause, pressureWakeEpoch>>

Resident == ready \o session \o submitted \o completed
SessionCanCharge(source) ==
  /\ Len(session) < MaxSessionSources
  /\ SeqPages(session) + ReservationPages(source) <= MaxSessionPages

Init ==
  /\ nextSource = 1
  /\ ready = <<>>
  /\ session = <<>>
  /\ submitted = <<>>
  /\ completed = <<>>
  /\ reclaimed = 0
  /\ leaseActive = FALSE
  /\ leaseGeneration = 0
  /\ capPending = FALSE
  /\ submittedGroups = <<>>
  /\ rollbackSources = <<>>
  /\ boundaryCause = "None"
  /\ pressureWakeEpoch = 0

PublishOrdinary ==
  /\ nextSource <= MaxSources
  /\ PayloadPages(nextSource) <= OrdinaryMaxPages
  /\ Len(Resident) < HighWaterSources
  /\ SeqPages(Resident) + ReservationPages(nextSource) <= HighWaterPages
  /\ Len(ready) < MaxReady
  /\ (~leaseActive \/ Len(ready) < SuccessorHeadroomSources)
  /\ ready' = Append(ready, nextSource)
  /\ nextSource' = nextSource + 1
  /\ UNCHANGED <<session, submitted, completed, reclaimed, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch>>

AcquireLease ==
  /\ ~leaseActive
  /\ session = <<>>
  /\ ready # <<>>
  /\ PayloadPages(Head(ready)) <= OrdinaryMaxPages
  /\ leaseGeneration < MaxLeaseGeneration
  /\ Len(submitted \o completed) + MaxSessionSources +
       SuccessorHeadroomSources <= HighWaterSources
  /\ SeqPages(submitted \o completed) + MaxSessionPages +
       SuccessorHeadroomPages <= HighWaterPages
  /\ leaseActive' = TRUE
  /\ leaseGeneration' = leaseGeneration + 1
  /\ UNCHANGED <<nextSource, ready, session, submitted, completed,
                  reclaimed, capPending, submittedGroups, rollbackSources,
                  boundaryCause, pressureWakeEpoch>>

AdmitReadyHead ==
  /\ leaseActive
  /\ ~capPending
  /\ ready # <<>>
  /\ SessionCanCharge(Head(ready))
  /\ session' = Append(session, Head(ready))
  /\ ready' = Tail(ready)
  /\ UNCHANGED <<nextSource, submitted, completed, reclaimed, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch>>

PostSessionCap ==
  /\ leaseActive
  /\ ~capPending
  /\ session # <<>>
  /\ ready # <<>>
  /\ ~SessionCanCharge(Head(ready))
  /\ capPending' = TRUE
  /\ boundaryCause' = "Cap"
  /\ UNCHANGED <<nextSource, ready, session, submitted, completed,
                  reclaimed, leaseActive, leaseGeneration, submittedGroups,
                  rollbackSources, pressureWakeEpoch>>

PostSemanticRelease ==
  /\ leaseActive
  /\ ~capPending
  /\ session # <<>>
  /\ capPending' = TRUE
  /\ boundaryCause' = "Semantic"
  /\ UNCHANGED <<nextSource, ready, session, submitted, completed,
                  reclaimed, leaseActive, leaseGeneration, submittedGroups,
                  rollbackSources, pressureWakeEpoch>>

PressureWake ==
  /\ pressureWakeEpoch < MaxSources
  /\ pressureWakeEpoch' = pressureWakeEpoch + 1
  /\ UNCHANGED <<nextSource, ready, session, submitted, completed,
                  reclaimed, leaseActive, leaseGeneration, capPending,
                  submittedGroups, rollbackSources, boundaryCause>>

SubmitPredecessor ==
  /\ capPending
  /\ session # <<>>
  /\ submitted' = submitted \o session
  /\ submittedGroups' = Append(submittedGroups, session)
  /\ session' = <<>>
  /\ leaseActive' = FALSE
  /\ capPending' = FALSE
  /\ boundaryCause' = "None"
  /\ UNCHANGED <<nextSource, ready, completed, reclaimed, leaseGeneration,
                  rollbackSources, pressureWakeEpoch>>

CompleteSubmitted ==
  /\ submitted # <<>>
  /\ completed' = Append(completed, Head(submitted))
  /\ submitted' = Tail(submitted)
  /\ UNCHANGED <<nextSource, ready, session, reclaimed, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch>>

ReclaimCompleted ==
  /\ completed # <<>>
  /\ Head(completed) = reclaimed + 1
  /\ reclaimed' = Head(completed)
  /\ completed' = Tail(completed)
  /\ UNCHANGED <<nextSource, ready, session, submitted, leaseActive,
                  leaseGeneration, capPending, submittedGroups,
                  rollbackSources, boundaryCause, pressureWakeEpoch>>

Next ==
  \/ PublishOrdinary
  \/ AcquireLease
  \/ AdmitReadyHead
  \/ PostSessionCap
  \/ PostSemanticRelease
  \/ PressureWake
  \/ SubmitPredecessor
  \/ CompleteSubmitted
  \/ ReclaimCompleted

Spec == Init /\ [][Next]_vars
  /\ WF_vars(AcquireLease)
  /\ WF_vars(AdmitReadyHead)
  /\ WF_vars(PostSessionCap)
  /\ WF_vars(SubmitPredecessor)
  /\ WF_vars(CompleteSubmitted)
  /\ WF_vars(ReclaimCompleted)

TypeOK ==
  /\ nextSource \in 1 .. (MaxSources + 1)
  /\ ready \in Seq(SourceIds)
  /\ session \in Seq(SourceIds)
  /\ submitted \in Seq(SourceIds)
  /\ completed \in Seq(SourceIds)
  /\ reclaimed \in SourceIds0
  /\ leaseActive \in BOOLEAN
  /\ leaseGeneration \in 0 .. MaxLeaseGeneration
  /\ capPending \in BOOLEAN
  /\ submittedGroups \in Seq(Seq(SourceIds))
  /\ rollbackSources \in Seq(SourceIds)
  /\ boundaryCause \in {"None", "Cap", "Semantic", "Pressure"}
  /\ pressureWakeEpoch \in 0 .. MaxSources

BoundedCapacity ==
  /\ Len(ready) <= MaxReady
  /\ Len(Resident) <= HighWaterSources
  /\ SeqPages(Resident) <= HighWaterPages
  /\ Len(session) <= MaxSessionSources
  /\ SeqPages(session) <= MaxSessionPages

LeaseOwnsCompleteHeadroom ==
  /\ MaxSessionSources + SuccessorHeadroomSources <= HighWaterSources
  /\ MaxSessionPages + SuccessorHeadroomPages <= HighWaterPages
  /\ SuccessorHeadroomSources >= 1
  /\ SuccessorHeadroomPages >= 2 * OrdinaryMaxPages - 1
  /\ (leaseActive => leaseGeneration > 0)
  /\ (leaseActive =>
        Len(ready) <= MaxSessionSources + SuccessorHeadroomSources)
  /\ (leaseActive =>
        SeqPages(ready) <= MaxSessionPages + SuccessorHeadroomPages)

CapCandidateStaysReady ==
  capPending /\ boundaryCause = "Cap" /\ ready # <<>> /\
    ~SessionCanCharge(Head(ready))
    => Head(ready) \notin {session[i] : i \in DOMAIN session}

NoPressureCreatedRelease ==
  /\ (capPending <=> boundaryCause # "None")
  /\ boundaryCause # "Pressure"

SubmittedGroupsRespectCap ==
  \A group \in {submittedGroups[i] : i \in DOMAIN submittedGroups} :
    /\ Len(group) <= MaxSessionSources
    /\ SeqPages(group) <= MaxSessionPages

Safety ==
  /\ TypeOK
  /\ BoundedCapacity
  /\ LeaseOwnsCompleteHeadroom
  /\ CapCandidateStaysReady
  /\ NoPressureCreatedRelease
  /\ SubmittedGroupsRespectCap

CapProgress == capPending ~> ~capPending

====
