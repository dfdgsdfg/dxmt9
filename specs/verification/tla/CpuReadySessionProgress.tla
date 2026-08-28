---- MODULE CpuReadySessionProgress ----
(***************************************************************************
 * Bounded CPU-ready Tape / EncodeSession progress refinement.
 *
 * This model deliberately abstracts payload bytes, Metal commands, and the
 * individual ordered-control reasons into two required-action classes:
 * Submit and PassClose.  It retains the scheduling facts that matter here:
 * a bounded FIFO, one exact pre-effect tentative prefix, and one pointer-free
 * deferred terminal suffix with an exact Writing successor. Park, Join,
 * StaleFailOpen, and semantic drain preserve queue/latch fences, session
 * effects and acknowledgement, ordered completion/reclaim, and a terminal
 * shutdown fence. Production binds that terminal action to final WSI
 * quiescence through SessionReleaseState::requestTerminal; GPU settlement
 * remains an explicit environment action.
 *
 * Environment actions (Publish, PostOrdinaryRelease, and RequestShutdown) are
 * not fair. Pressure latches remain in the state type only as a refinement
 * guard for the retired predecessor; no action can activate them. The
 * liveness claims below are conditional
 * on an event having been posted.  Weak fairness is applied only to enabled
 * coordinator, completion, and finish-thread actions.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxSources, MaxReady, MaxResident, MaxBatch, MaxSessionLen,
          MaxReleaseEvents, MaxReleaseGeneration, MaxPressureGeneration

SourceIds == 1 .. MaxSources
SourceIds0 == 0 .. MaxSources
ValidationOutcomes == {"Pass", "Fail"}
ReleaseKinds == {"Flush", "Semantic"}
RequiredAction(kind) == IF kind = "Semantic" THEN "PassClose" ELSE "Submit"
ReleaseActions == {"Submit", "PassClose"}
PressureKinds == {"None", "Admission", "Writer"}

ReleaseEvent ==
  [kind : ReleaseKinds,
   action : ReleaseActions,
   fence : SourceIds0,
   generation : 1 .. MaxReleaseGeneration]

PressureLatch ==
  [active : BOOLEAN,
   fence : SourceIds0,
   generation : 0 .. MaxPressureGeneration]

PressureAck ==
  [kind : PressureKinds,
   requestedGeneration : 0 .. MaxPressureGeneration,
   liveGeneration : 0 .. MaxPressureGeneration,
   accepted : BOOLEAN]

CapacityWakeState ==
  [phase : {"Inactive", "NeedDenial", "Waiting", "Woken",
            "DirectStarted"},
   generation : 0 .. MaxSources,
   observedGeneration : 0 .. MaxSources]

DeferredPhases ==
  {"Empty", "PrefixEncoded", "Held", "SuccessorTentative",
   "JoinEffectful"}
DeferredProvenances == {"None", "Ordinary", "NaturalAfterMerge"}
DeferredDrainReasons ==
  {"None", "Pressure", "Release", "Control", "Initializer", "Stop",
   "Loss", "WriterLoss", "Headroom", "Lease"}

DeferredSuffixState ==
  [phase : DeferredPhases,
   current : SourceIds0,
   currentGeneration : SourceIds0,
   provenance : DeferredProvenances,
   expectedSuccessor : SourceIds0,
   successorGeneration : SourceIds0,
   releaseIdentity : 0 .. MaxReleaseGeneration,
   leaseIdentity : SourceIds0,
   liveSuccessorGeneration : SourceIds0,
   liveLeaseGeneration : SourceIds0,
   writingSuccessor : SourceIds0,
   writerWakePending : BOOLEAN,
   drainReason : DeferredDrainReasons,
   borrowedOwner : BOOLEAN,
   currentPrefixEffects : 0 .. 1,
   currentSuffixEffects : 0 .. 1,
   successorHeadEffects : 0 .. 1,
   successorEffectful : BOOLEAN,
   lastRestored : SourceIds0,
   rollbackAfterEffect : BOOLEAN]

VARIABLES
  nextSource,
  ready,
  tentative,
  tentativeBaseSuffix,
  tentativeValidation,
  validationFailedOnce,
  session,
  passOpen,
  submitted,
  completed,
  reclaimed,
  effectsThrough,
  passClosedThrough,
  releaseQ,
  releaseGeneration,
  ordinaryAckGeneration,
  ackedSubmitFences,
  ackedPassCloseFences,
  admissionLatch,
  writerLatch,
  admissionAckGeneration,
  writerAckGeneration,
  lastPressureAck,
  shutdownRequested,
  terminalFence,
  terminalAck,
  shutdownComplete,
  lastRollbackPrefix,
  lastRollbackSuffix,
  lastRollbackReady,
  lastRollbackPreEffect,
  capacityWake,
  deferredSuffix

vars ==
  <<nextSource, ready, tentative, tentativeBaseSuffix,
    tentativeValidation, validationFailedOnce, session, passOpen, submitted,
    completed, reclaimed, effectsThrough, passClosedThrough, releaseQ,
    releaseGeneration, ordinaryAckGeneration, ackedSubmitFences,
    ackedPassCloseFences, admissionLatch, writerLatch,
    admissionAckGeneration, writerAckGeneration, lastPressureAck,
    shutdownRequested, terminalFence, terminalAck, shutdownComplete,
    lastRollbackPrefix, lastRollbackSuffix, lastRollbackReady,
    lastRollbackPreEffect, capacityWake, deferredSuffix>>

SeqSet(seq) == {seq[i] : i \in DOMAIN seq}

Prefix(seq, count) == [i \in 1 .. count |-> seq[i]]
Suffix(seq, count) ==
  [i \in 1 .. (Len(seq) - count) |-> seq[count + i]]

IsPrefix(prefix, seq) ==
  /\ Len(prefix) <= Len(seq)
  /\ \A i \in DOMAIN prefix : prefix[i] = seq[i]

IsConsecutive(seq) ==
  \A i \in DOMAIN seq : i < Len(seq) => seq[i + 1] = seq[i] + 1

DeferredTentativeOrder ==
  IF deferredSuffix.phase \in {"SuccessorTentative", "JoinEffectful"}
  THEN <<deferredSuffix.expectedSuccessor>>
  ELSE <<>>
DeferredWritingOrder ==
  IF deferredSuffix.writingSuccessor = 0
  THEN <<>>
  ELSE <<deferredSuffix.writingSuccessor>>
LiveOrder == completed \o submitted \o session \o tentative \o
             DeferredTentativeOrder \o ready \o DeferredWritingOrder
ResidentCount == nextSource - 1 - reclaimed
CompletedThrough == reclaimed + Len(completed)
SubmittedThrough == CompletedThrough + Len(submitted)
RepresentedThrough == SubmittedThrough + Len(session) + Len(tentative)

Min2(a, b) == IF a <= b THEN a ELSE b
OrdinaryFence == IF Len(releaseQ) = 0 THEN MaxSources
                   ELSE Head(releaseQ).fence
AdmissionFence == IF admissionLatch.active THEN admissionLatch.fence
                    ELSE MaxSources
WriterFence == IF writerLatch.active THEN writerLatch.fence ELSE MaxSources
TerminalFence == IF shutdownRequested THEN terminalFence ELSE MaxSources
EarliestFence ==
  Min2(Min2(OrdinaryFence, AdmissionFence),
       Min2(WriterFence, TerminalFence))

ReleasePending ==
  Len(releaseQ) > 0 \/ admissionLatch.active \/ writerLatch.active \/
  (shutdownRequested /\ ~terminalAck)

SubmitReleasePending ==
  /\ ReleasePending
  /\ \/ Len(session) = MaxSessionLen
     \/ admissionLatch.active
     \/ writerLatch.active
     \/ (shutdownRequested /\ ~terminalAck)
     \/ (Len(releaseQ) > 0 /\ Head(releaseQ).action = "Submit")

EmptyLatch == [active |-> FALSE, fence |-> 0, generation |-> 0]
EmptyPressureAck ==
  [kind |-> "None", requestedGeneration |-> 0,
   liveGeneration |-> 0, accepted |-> FALSE]
EmptyDeferredSuffix ==
  [phase |-> "Empty", current |-> 0, currentGeneration |-> 0,
   provenance |-> "None", expectedSuccessor |-> 0,
   successorGeneration |-> 0, releaseIdentity |-> 0,
   leaseIdentity |-> 0, liveSuccessorGeneration |-> 0,
   liveLeaseGeneration |-> 0, writingSuccessor |-> 0,
   writerWakePending |-> FALSE,
   drainReason |-> "None", borrowedOwner |-> FALSE,
   currentPrefixEffects |-> 0, currentSuffixEffects |-> 0,
   successorHeadEffects |-> 0, successorEffectful |-> FALSE,
   lastRestored |-> 0, rollbackAfterEffect |-> FALSE]

Init ==
  /\ \/ /\ nextSource = 1
         /\ ready = <<>>
         /\ submitted = <<>>
         /\ effectsThrough = 0
         /\ passClosedThrough = 0
         /\ deferredSuffix = EmptyDeferredSuffix
         /\ capacityWake =
              [phase |-> "Inactive", generation |-> 0,
               observedGeneration |-> 0]
     \/ /\ nextSource = 1
         /\ ready = <<>>
         /\ submitted = <<>>
         /\ effectsThrough = 0
         /\ passClosedThrough = 0
         /\ deferredSuffix = EmptyDeferredSuffix
         /\ capacityWake =
              [phase |-> "NeedDenial", generation |-> 0,
               observedGeneration |-> 0]
     \/ \E provenance \in {"Ordinary", "NaturalAfterMerge"} :
          /\ nextSource = 3
          /\ ready = <<>>
          /\ submitted = <<>>
          /\ effectsThrough = 1
          /\ passClosedThrough = 0
          /\ capacityWake =
               [phase |-> "Inactive", generation |-> 0,
                observedGeneration |-> 0]
          /\ deferredSuffix =
               [EmptyDeferredSuffix EXCEPT
                  !.phase = "PrefixEncoded",
                  !.current = 1,
                  !.currentGeneration = 1,
                  !.provenance = provenance,
                  !.expectedSuccessor = 2,
                  !.successorGeneration = 1,
                  !.leaseIdentity = 1,
                  !.liveSuccessorGeneration = 1,
                  !.liveLeaseGeneration = 1,
                  !.writingSuccessor = 2,
                  !.currentPrefixEffects = 1]
  /\ tentative = <<>>
  /\ tentativeBaseSuffix = <<>>
  /\ tentativeValidation = "Pass"
  /\ validationFailedOnce = {}
  /\ session =
       IF deferredSuffix.phase = "PrefixEncoded" THEN <<1>> ELSE <<>>
  /\ passOpen = (deferredSuffix.phase = "PrefixEncoded")
  /\ completed = <<>>
  /\ reclaimed = 0
  /\ releaseQ = <<>>
  /\ releaseGeneration = 0
  /\ ordinaryAckGeneration = 0
  /\ ackedSubmitFences = {}
  /\ ackedPassCloseFences = {}
  /\ admissionLatch = EmptyLatch
  /\ writerLatch = EmptyLatch
  /\ admissionAckGeneration = 0
  /\ writerAckGeneration = 0
  /\ lastPressureAck = EmptyPressureAck
  /\ shutdownRequested = FALSE
  /\ terminalFence = 0
  /\ terminalAck = FALSE
  /\ shutdownComplete = FALSE
  /\ lastRollbackPrefix = <<>>
  /\ lastRollbackSuffix = <<>>
  /\ lastRollbackReady = <<>>
  /\ lastRollbackPreEffect = TRUE

SeedStartupCapacityBlocker ==
  /\ capacityWake.phase = "NeedDenial"
  /\ nextSource = 1
  /\ ready = <<>>
  /\ tentative = <<>>
  /\ session = <<>>
  /\ submitted = <<>>
  /\ completed = <<>>
  /\ reclaimed = 0
  /\ nextSource' = 3
  /\ ready' = <<2>>
  /\ submitted' = <<1>>
  /\ effectsThrough' = 1
  /\ passClosedThrough' = 1
  /\ capacityWake' =
       [capacityWake EXCEPT
          !.phase = "Waiting",
          !.observedGeneration = capacityWake.generation]
  /\ UNCHANGED <<tentative, tentativeBaseSuffix, tentativeValidation,
                  validationFailedOnce, session, passOpen, completed,
                  reclaimed, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, deferredSuffix>>

Publish ==
  /\ ~shutdownRequested
  /\ ~admissionLatch.active
  /\ ~writerLatch.active
  /\ nextSource <= MaxSources
  /\ ResidentCount < MaxResident
  /\ Len(ready) < MaxReady
  /\ ready' = Append(ready, nextSource)
  /\ nextSource' = nextSource + 1
  /\ UNCHANGED <<tentative, tentativeBaseSuffix, tentativeValidation,
                  validationFailedOnce, session, passOpen, submitted,
                  completed, reclaimed, effectsThrough, passClosedThrough,
                  releaseQ, releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

ReservePrefix(count, outcome) ==
  /\ tentative = <<>>
  /\ capacityWake.phase # "Waiting"
  /\ count \in 1 .. MaxBatch
  /\ count <= Len(ready)
  /\ count + Len(session) <= MaxSessionLen
  /\ ready[count] <= EarliestFence
  /\ outcome \in ValidationOutcomes
  /\ outcome = "Pass" \/ ready[1] \notin validationFailedOnce
  /\ tentative' = Prefix(ready, count)
  /\ tentativeBaseSuffix' = Suffix(ready, count)
  /\ tentativeValidation' = outcome
  /\ ready' = Suffix(ready, count)
  /\ UNCHANGED <<nextSource, validationFailedOnce, session, passOpen,
                  submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

ReserveAny ==
  \E count \in 1 .. MaxBatch, outcome \in ValidationOutcomes :
    ReservePrefix(count, outcome)

CommitTentative ==
  /\ tentative # <<>>
  /\ tentativeValidation = "Pass"
  /\ session' = session \o tentative
  /\ effectsThrough' = effectsThrough + Len(tentative)
  /\ passOpen' = TRUE
  /\ tentative' = <<>>
  /\ tentativeBaseSuffix' = <<>>
  /\ tentativeValidation' = "Pass"
  /\ capacityWake' =
       IF capacityWake.phase = "Woken" /\ 2 \in SeqSet(tentative)
       THEN [capacityWake EXCEPT !.phase = "DirectStarted"]
       ELSE capacityWake
  /\ UNCHANGED <<nextSource, ready, validationFailedOnce, submitted,
                  completed, reclaimed, passClosedThrough, releaseQ,
                  releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, deferredSuffix>>

RollbackTentative ==
  /\ tentative # <<>>
  /\ tentativeValidation = "Fail"
  /\ ready' = tentative \o ready
  /\ validationFailedOnce' =
       validationFailedOnce \cup SeqSet(tentative)
  /\ lastRollbackPrefix' = tentative
  /\ lastRollbackSuffix' = ready
  /\ lastRollbackReady' = tentative \o ready
  /\ lastRollbackPreEffect' = (effectsThrough < Head(tentative))
  /\ tentative' = <<>>
  /\ tentativeBaseSuffix' = <<>>
  /\ tentativeValidation' = "Pass"
  /\ UNCHANGED <<nextSource, session, passOpen, submitted, completed,
                  reclaimed, effectsThrough, passClosedThrough, releaseQ,
                  releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, capacityWake,
                  deferredSuffix>>

ParkDeferredSuffix ==
  /\ deferredSuffix.phase = "PrefixEncoded"
  /\ deferredSuffix.current = 1
  /\ deferredSuffix.expectedSuccessor = 2
  /\ deferredSuffix.writingSuccessor = 2
  /\ deferredSuffix.releaseIdentity = releaseGeneration
  /\ deferredSuffix.leaseIdentity = deferredSuffix.liveLeaseGeneration
  /\ ~ReleasePending
  /\ deferredSuffix' = [deferredSuffix EXCEPT !.phase = "Held"]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

WriterPublishDeferredSuccessor ==
  /\ deferredSuffix.phase \in {"Held", "Empty"}
  /\ deferredSuffix.current # 0
  /\ deferredSuffix.writingSuccessor = deferredSuffix.expectedSuccessor
  /\ (deferredSuffix.phase = "Held" \/
      deferredSuffix.writerWakePending)
  /\ Len(ready) < MaxReady
  /\ ready' = Append(ready, deferredSuffix.writingSuccessor)
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.writingSuccessor = 0,
          !.writerWakePending = FALSE]
  /\ UNCHANGED <<nextSource, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

ReserveDeferredSuccessor ==
  /\ deferredSuffix.phase = "Held"
  /\ deferredSuffix.writingSuccessor = 0
  /\ ready # <<>>
  /\ Head(ready) = deferredSuffix.expectedSuccessor
  /\ ~ReleasePending
  /\ deferredSuffix.drainReason \in {"None", "Pressure"}
  /\ deferredSuffix.currentGeneration = 1
  /\ deferredSuffix.successorGeneration =
       deferredSuffix.liveSuccessorGeneration
  /\ deferredSuffix.releaseIdentity = releaseGeneration
  /\ deferredSuffix.leaseIdentity = deferredSuffix.liveLeaseGeneration
  /\ ready' = Tail(ready)
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT !.phase = "SuccessorTentative"]
  /\ UNCHANGED <<nextSource, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

RequestDeferredDrain(reason) ==
  /\ deferredSuffix.phase \in {"Held", "SuccessorTentative"}
  /\ deferredSuffix.drainReason = "None"
  /\ reason \in DeferredDrainReasons \ {"None"}
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.drainReason = reason,
          !.liveLeaseGeneration =
             IF reason = "Lease" THEN @ + 1 ELSE @]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

RequestAnyDeferredDrain ==
  \E reason \in DeferredDrainReasons \ {"None"} :
    RequestDeferredDrain(reason)

StaleFailOpen ==
  /\ deferredSuffix.phase = "SuccessorTentative"
  /\ ~deferredSuffix.successorEffectful
  /\ deferredSuffix.writingSuccessor = 0
  /\ deferredSuffix.successorGeneration =
       deferredSuffix.liveSuccessorGeneration
  /\ deferredSuffix.leaseIdentity # deferredSuffix.liveLeaseGeneration
  /\ ready' = <<deferredSuffix.expectedSuccessor>> \o ready
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.phase = "Empty",
          !.drainReason = "None",
          !.currentSuffixEffects = 1,
          !.lastRestored = deferredSuffix.expectedSuccessor]
  /\ UNCHANGED <<nextSource, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

JoinDeferredSuccessor ==
  /\ deferredSuffix.phase = "SuccessorTentative"
  /\ ~ReleasePending
  /\ deferredSuffix.drainReason \in {"None", "Pressure"}
  /\ deferredSuffix.currentGeneration = 1
  /\ deferredSuffix.successorGeneration =
       deferredSuffix.liveSuccessorGeneration
  /\ deferredSuffix.releaseIdentity = releaseGeneration
  /\ deferredSuffix.leaseIdentity = deferredSuffix.liveLeaseGeneration
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.phase = "JoinEffectful",
          !.successorHeadEffects = 1,
          !.successorEffectful = TRUE]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

CompleteDeferredJoin ==
  /\ deferredSuffix.phase = "JoinEffectful"
  /\ deferredSuffix.successorEffectful
  /\ deferredSuffix.currentSuffixEffects = 0
  /\ session' = Append(session, deferredSuffix.expectedSuccessor)
  /\ effectsThrough' = effectsThrough + 1
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.phase = "Empty",
          !.drainReason = "None",
          !.currentSuffixEffects = 1]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, passOpen,
                  submitted, completed, reclaimed, passClosedThrough,
                  releaseQ, releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

ExactDeferredSuccessorAvailable ==
  /\ deferredSuffix.writingSuccessor = 0
  /\ \/ deferredSuffix.phase = "SuccessorTentative"
     \/ /\ deferredSuffix.phase = "Held"
        /\ ready # <<>>
        /\ Head(ready) = deferredSuffix.expectedSuccessor

DrainDeferredSuffix ==
  /\ deferredSuffix.phase \in {"Held", "SuccessorTentative"}
  /\ \/ ReleasePending
     \/ deferredSuffix.drainReason # "None"
  /\ ~(deferredSuffix.drainReason = "Pressure" /\
       ExactDeferredSuccessorAvailable)
  /\ ready' =
       IF deferredSuffix.phase = "SuccessorTentative"
       THEN <<deferredSuffix.expectedSuccessor>> \o ready
       ELSE ready
  /\ deferredSuffix' =
       [deferredSuffix EXCEPT
          !.phase = "Empty",
          !.writerWakePending =
             deferredSuffix.phase = "Held" /\
             deferredSuffix.writingSuccessor # 0,
          !.drainReason = "None",
          !.currentSuffixEffects = 1,
          !.lastRestored =
             IF deferredSuffix.phase = "SuccessorTentative"
             THEN deferredSuffix.expectedSuccessor ELSE @]
  /\ UNCHANGED <<nextSource, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake>>

ClosePass ==
  /\ passOpen
  /\ session # <<>>
  /\ ReleasePending
  /\ passOpen' = FALSE
  /\ passClosedThrough' = effectsThrough
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  submitted, completed, reclaimed, effectsThrough, releaseQ,
                  releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

SubmitSession ==
  /\ session # <<>>
  /\ ~passOpen
  /\ SubmitReleasePending
  /\ submitted' = submitted \o session
  /\ session' = <<>>
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, passOpen, completed,
                  reclaimed, effectsThrough, passClosedThrough, releaseQ,
                  releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

PostOrdinaryRelease(kind) ==
  /\ ~shutdownRequested
  /\ kind \in ReleaseKinds
  /\ Len(releaseQ) < MaxReleaseEvents
  /\ releaseGeneration < MaxReleaseGeneration
  /\ releaseQ' =
       Append(releaseQ,
              [kind |-> kind,
               action |-> RequiredAction(kind),
               fence |-> nextSource - 1,
               generation |-> releaseGeneration + 1])
  /\ releaseGeneration' = releaseGeneration + 1
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence, terminalAck,
                  shutdownComplete, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

PostAnyOrdinaryRelease == \E kind \in ReleaseKinds : PostOrdinaryRelease(kind)

AckOrdinaryRelease ==
  /\ Len(releaseQ) > 0
  /\ LET event == Head(releaseQ) IN
       /\ IF event.action = "Submit"
             THEN event.fence <= SubmittedThrough
             ELSE event.fence <= passClosedThrough
       /\ releaseQ' = Tail(releaseQ)
       /\ ordinaryAckGeneration' = event.generation
       /\ ackedSubmitFences' =
            IF event.action = "Submit"
            THEN ackedSubmitFences \cup {event.fence}
            ELSE ackedSubmitFences
       /\ ackedPassCloseFences' =
            IF event.action = "PassClose"
            THEN ackedPassCloseFences \cup {event.fence}
            ELSE ackedPassCloseFences
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseGeneration, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

PostAdmissionPressure ==
  /\ ~shutdownRequested
  /\ ~admissionLatch.active
  /\ admissionLatch.generation < MaxPressureGeneration
  /\ ResidentCount = MaxResident
  /\ admissionLatch' =
       [active |-> TRUE, fence |-> nextSource - 1,
        generation |-> admissionLatch.generation + 1]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

PostWriterPressure ==
  /\ ~shutdownRequested
  /\ ~writerLatch.active
  /\ writerLatch.generation < MaxPressureGeneration
  /\ ResidentCount = MaxResident
  /\ writerLatch' =
       [active |-> TRUE, fence |-> nextSource - 1,
        generation |-> writerLatch.generation + 1]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

AckAdmissionPressure ==
  /\ admissionLatch.active
  /\ admissionLatch.fence <= SubmittedThrough
  /\ admissionLatch' =
       [admissionLatch EXCEPT !.active = FALSE]
  /\ admissionAckGeneration' = admissionLatch.generation
  /\ ackedSubmitFences' =
       ackedSubmitFences \cup {admissionLatch.fence}
  /\ lastPressureAck' =
       [kind |-> "Admission",
        requestedGeneration |-> admissionLatch.generation,
        liveGeneration |-> admissionLatch.generation,
        accepted |-> TRUE]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedPassCloseFences, writerLatch,
                  writerAckGeneration, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

AckWriterPressure ==
  /\ writerLatch.active
  /\ writerLatch.fence <= SubmittedThrough
  /\ writerLatch' = [writerLatch EXCEPT !.active = FALSE]
  /\ writerAckGeneration' = writerLatch.generation
  /\ ackedSubmitFences' = ackedSubmitFences \cup {writerLatch.fence}
  /\ lastPressureAck' =
       [kind |-> "Writer",
        requestedGeneration |-> writerLatch.generation,
        liveGeneration |-> writerLatch.generation,
        accepted |-> TRUE]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedPassCloseFences,
                  admissionLatch, admissionAckGeneration,
                  shutdownRequested, terminalFence, terminalAck,
                  shutdownComplete, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

RejectStaleAdmissionAck(generation) ==
  /\ admissionLatch.active
  /\ generation \in 0 .. MaxPressureGeneration
  /\ generation # admissionLatch.generation
  /\ lastPressureAck' =
       [kind |-> "Admission", requestedGeneration |-> generation,
        liveGeneration |-> admissionLatch.generation, accepted |-> FALSE]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  shutdownRequested, terminalFence, terminalAck,
                  shutdownComplete, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

RejectStaleWriterAck(generation) ==
  /\ writerLatch.active
  /\ generation \in 0 .. MaxPressureGeneration
  /\ generation # writerLatch.generation
  /\ lastPressureAck' =
       [kind |-> "Writer", requestedGeneration |-> generation,
        liveGeneration |-> writerLatch.generation, accepted |-> FALSE]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  shutdownRequested, terminalFence, terminalAck,
                  shutdownComplete, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

RejectAnyStalePressureAck ==
  (\E generation \in 0 .. MaxPressureGeneration :
      RejectStaleAdmissionAck(generation))
  \/ (\E generation \in 0 .. MaxPressureGeneration :
        RejectStaleWriterAck(generation))

RequestShutdown ==
  /\ ~shutdownRequested
  /\ shutdownRequested' = TRUE
  /\ terminalFence' = nextSource - 1
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, terminalAck, shutdownComplete,
                  lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

AckTerminalFence ==
  /\ shutdownRequested
  /\ ~terminalAck
  /\ Len(releaseQ) = 0
  /\ ~admissionLatch.active
  /\ ~writerLatch.active
  /\ terminalFence <= SubmittedThrough
  /\ terminalAck' = TRUE
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  shutdownComplete, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

CompleteSubmitted ==
  /\ submitted # <<>>
  /\ completed' = Append(completed, Head(submitted))
  /\ submitted' = Tail(submitted)
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, reclaimed, effectsThrough, passClosedThrough,
                  releaseQ, releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, capacityWake, deferredSuffix>>

ReclaimCompleted ==
  /\ completed # <<>>
  /\ Head(completed) = reclaimed + 1
  /\ reclaimed' = Head(completed)
  /\ completed' = Tail(completed)
  /\ capacityWake' =
       [capacityWake EXCEPT !.generation = @ + 1]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, effectsThrough, passClosedThrough,
                  releaseQ, releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, deferredSuffix>>

WakeCapacityLease ==
  /\ capacityWake.phase = "Waiting"
  /\ capacityWake.generation # capacityWake.observedGeneration
  /\ capacityWake' = [capacityWake EXCEPT !.phase = "Woken"]
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect, deferredSuffix>>

FinishShutdown ==
  /\ shutdownRequested
  /\ terminalAck
  /\ reclaimed = terminalFence
  /\ ready = <<>>
  /\ tentative = <<>>
  /\ session = <<>>
  /\ submitted = <<>>
  /\ completed = <<>>
  /\ ~shutdownComplete
  /\ shutdownComplete' = TRUE
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, completed, reclaimed, effectsThrough,
                  passClosedThrough, releaseQ, releaseGeneration,
                  ordinaryAckGeneration, ackedSubmitFences,
                  ackedPassCloseFences, admissionLatch, writerLatch,
                  admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, lastRollbackPrefix, lastRollbackSuffix,
                  lastRollbackReady, lastRollbackPreEffect, capacityWake,
                  deferredSuffix>>

Next ==
  IF capacityWake.phase = "NeedDenial"
  THEN SeedStartupCapacityBlocker
  ELSE IF deferredSuffix.phase = "PrefixEncoded"
  THEN ParkDeferredSuffix
  ELSE IF deferredSuffix.phase = "Held"
  THEN \/ WriterPublishDeferredSuccessor
       \/ ReserveDeferredSuccessor
       \/ RequestAnyDeferredDrain
       \/ PostAnyOrdinaryRelease
       \/ RequestShutdown
       \/ DrainDeferredSuffix
  ELSE IF deferredSuffix.phase = "SuccessorTentative"
  THEN \/ JoinDeferredSuccessor
       \/ StaleFailOpen
       \/ RequestAnyDeferredDrain
       \/ PostAnyOrdinaryRelease
       \/ RequestShutdown
       \/ DrainDeferredSuffix
  ELSE IF deferredSuffix.phase = "JoinEffectful"
  THEN CompleteDeferredJoin
  ELSE \/ Publish
       \/ WriterPublishDeferredSuccessor
       \/ ReserveAny
       \/ CommitTentative
       \/ RollbackTentative
       \/ ClosePass
       \/ SubmitSession
       \/ PostAnyOrdinaryRelease
       \/ AckOrdinaryRelease
       \/ RequestShutdown
       \/ AckTerminalFence
       \/ CompleteSubmitted
       \/ ReclaimCompleted
       \/ WakeCapacityLease
       \/ FinishShutdown

SystemFairness ==
  /\ WF_vars(ReserveAny)
  /\ WF_vars(CommitTentative)
  /\ WF_vars(RollbackTentative)
  /\ WF_vars(ClosePass)
  /\ WF_vars(SubmitSession)
  /\ WF_vars(AckOrdinaryRelease)
  /\ WF_vars(AckTerminalFence)
  /\ WF_vars(CompleteSubmitted)
  /\ WF_vars(ReclaimCompleted)
  /\ WF_vars(SeedStartupCapacityBlocker)
  /\ WF_vars(WakeCapacityLease)
  /\ WF_vars(FinishShutdown)
  /\ WF_vars(ParkDeferredSuffix)
  /\ WF_vars(WriterPublishDeferredSuccessor)
  /\ WF_vars(ReserveDeferredSuccessor)
  /\ WF_vars(StaleFailOpen)
  /\ WF_vars(JoinDeferredSuccessor)
  /\ WF_vars(CompleteDeferredJoin)
  /\ WF_vars(DrainDeferredSuffix)

Spec == Init /\ [][Next]_vars /\ SystemFairness

TypeOK ==
  /\ nextSource \in 1 .. (MaxSources + 1)
  /\ ready \in Seq(SourceIds)
  /\ tentative \in Seq(SourceIds)
  /\ tentativeBaseSuffix \in Seq(SourceIds)
  /\ tentativeValidation \in ValidationOutcomes
  /\ validationFailedOnce \subseteq SourceIds
  /\ session \in Seq(SourceIds)
  /\ passOpen \in BOOLEAN
  /\ submitted \in Seq(SourceIds)
  /\ completed \in Seq(SourceIds)
  /\ reclaimed \in SourceIds0
  /\ effectsThrough \in SourceIds0
  /\ passClosedThrough \in SourceIds0
  /\ releaseQ \in Seq(ReleaseEvent)
  /\ releaseGeneration \in 0 .. MaxReleaseGeneration
  /\ ordinaryAckGeneration \in 0 .. MaxReleaseGeneration
  /\ ackedSubmitFences \subseteq SourceIds0
  /\ ackedPassCloseFences \subseteq SourceIds0
  /\ admissionLatch \in PressureLatch
  /\ writerLatch \in PressureLatch
  /\ admissionAckGeneration \in 0 .. MaxPressureGeneration
  /\ writerAckGeneration \in 0 .. MaxPressureGeneration
  /\ lastPressureAck \in PressureAck
  /\ shutdownRequested \in BOOLEAN
  /\ terminalFence \in SourceIds0
  /\ terminalAck \in BOOLEAN
  /\ shutdownComplete \in BOOLEAN
  /\ lastRollbackPrefix \in Seq(SourceIds)
  /\ lastRollbackSuffix \in Seq(SourceIds)
  /\ lastRollbackReady \in Seq(SourceIds)
  /\ lastRollbackPreEffect \in BOOLEAN
  /\ capacityWake \in CapacityWakeState
  /\ deferredSuffix \in DeferredSuffixState

BoundedStores ==
  /\ ResidentCount <= MaxResident
  /\ Len(ready) <= MaxReady
  /\ Len(tentative) <= MaxBatch
  /\ Len(session) <= MaxSessionLen
  /\ Len(releaseQ) <= MaxReleaseEvents

FifoSourceOrder ==
  /\ Len(LiveOrder) = ResidentCount
  /\ \A i \in DOMAIN LiveOrder : LiveOrder[i] = reclaimed + i
  /\ IsConsecutive(ready)
  /\ IsConsecutive(tentative)
  /\ IsConsecutive(session)
  /\ IsConsecutive(submitted)
  /\ IsConsecutive(completed)

DeferredOwnerIsBoundedValue ==
  /\ ~deferredSuffix.borrowedOwner
  /\ (deferredSuffix.current = 0 <=>
        deferredSuffix.phase = "Empty" /\
        deferredSuffix.expectedSuccessor = 0)
  /\ (deferredSuffix.current # 0 =>
        /\ deferredSuffix.current = 1
        /\ deferredSuffix.expectedSuccessor = 2
        /\ deferredSuffix.currentGeneration = 1
        /\ deferredSuffix.provenance \in
             {"Ordinary", "NaturalAfterMerge"}
        /\ deferredSuffix.leaseIdentity > 0)

DeferredCommandsExactlyOnce ==
  /\ deferredSuffix.currentPrefixEffects <= 1
  /\ deferredSuffix.currentSuffixEffects <= 1
  /\ deferredSuffix.successorHeadEffects <= 1
  /\ (deferredSuffix.current # 0 =>
        deferredSuffix.currentPrefixEffects = 1)
  /\ (deferredSuffix.phase = "JoinEffectful" =>
        /\ deferredSuffix.successorHeadEffects = 1
        /\ deferredSuffix.currentSuffixEffects = 0)
  /\ (deferredSuffix.current # 0 /\
       deferredSuffix.phase = "Empty" =>
        deferredSuffix.currentSuffixEffects = 1)

DeferredIdentityAndEffectBoundary ==
  /\ (deferredSuffix.phase \in
        {"PrefixEncoded", "Held", "SuccessorTentative",
         "JoinEffectful"} =>
        /\ deferredSuffix.currentGeneration = 1
        /\ deferredSuffix.releaseIdentity <= releaseGeneration
        /\ deferredSuffix.leaseIdentity <=
             deferredSuffix.liveLeaseGeneration)
  /\ (deferredSuffix.phase = "JoinEffectful" =>
        /\ deferredSuffix.successorGeneration =
             deferredSuffix.liveSuccessorGeneration
        /\ deferredSuffix.releaseIdentity = releaseGeneration
        /\ deferredSuffix.leaseIdentity =
             deferredSuffix.liveLeaseGeneration
        /\ deferredSuffix.successorEffectful)
  /\ ~deferredSuffix.rollbackAfterEffect

OnlyUnaffectedSuccessorRestores ==
  /\ deferredSuffix.lastRestored \in
       {0, deferredSuffix.expectedSuccessor}
  /\ (deferredSuffix.lastRestored # 0 =>
        /\ deferredSuffix.successorHeadEffects = 0
        /\ deferredSuffix.writingSuccessor = 0
        /\ deferredSuffix.successorGeneration =
             deferredSuffix.liveSuccessorGeneration)

WriterOwnsDeferredPublication ==
  /\ (deferredSuffix.writerWakePending =>
        /\ deferredSuffix.phase = "Empty"
        /\ deferredSuffix.writingSuccessor =
             deferredSuffix.expectedSuccessor
        /\ deferredSuffix.expectedSuccessor \notin SeqSet(ready))
  /\ (deferredSuffix.writingSuccessor # 0 =>
        deferredSuffix.writingSuccessor =
          deferredSuffix.expectedSuccessor)

ExactSuccessorBeatsPressure ==
  ~(deferredSuffix.drainReason = "Pressure" /\
    ExactDeferredSuccessorAvailable /\
    deferredSuffix.currentSuffixEffects = 1)

ReleaseOrControlDrainsBeforeEffects ==
  deferredSuffix.phase = "JoinEffectful" =>
    /\ ~ReleasePending
    /\ deferredSuffix.drainReason \in {"None", "Pressure"}

OneTentativePrefix ==
  /\ (tentative = <<>> => tentativeBaseSuffix = <<>>)
  /\ (tentative # <<>> => IsPrefix(tentativeBaseSuffix, ready))
  /\ effectsThrough = SubmittedThrough + Len(session)

CompatibleSuffixStaysReady ==
  tentative = <<>> \/ IsPrefix(tentativeBaseSuffix, ready)

NoYoungerThanEarliestFence ==
  \A source \in SeqSet(session \o tentative) : source <= EarliestFence

RollbackBeforeEffectsRestoresExactPrefix ==
  lastRollbackPrefix = <<>>
  \/ /\ lastRollbackPreEffect
     /\ lastRollbackReady = lastRollbackPrefix \o lastRollbackSuffix

ReleaseQueueOrdered ==
  /\ ordinaryAckGeneration + Len(releaseQ) = releaseGeneration
  /\ \A i \in DOMAIN releaseQ :
       /\ releaseQ[i].generation = ordinaryAckGeneration + i
       /\ releaseQ[i].action = RequiredAction(releaseQ[i].kind)
       /\ (i < Len(releaseQ) =>
             releaseQ[i].fence <= releaseQ[i + 1].fence)

AckAfterRequiredActionAndFenceCoverage ==
  /\ \A fence \in ackedSubmitFences : fence <= SubmittedThrough
  /\ \A fence \in ackedPassCloseFences : fence <= passClosedThrough
  /\ ~terminalAck \/
       /\ shutdownRequested
       /\ terminalFence <= SubmittedThrough

NoCompletionBeforeSubmit ==
  /\ CompletedThrough <= SubmittedThrough
  /\ \A source \in SeqSet(completed) : source <= SubmittedThrough
  /\ reclaimed <= SubmittedThrough

NoStalePressureAck ==
  /\ admissionAckGeneration <= admissionLatch.generation
  /\ writerAckGeneration <= writerLatch.generation
  /\ (admissionLatch.active =>
        admissionAckGeneration < admissionLatch.generation)
  /\ (~admissionLatch.active =>
        admissionAckGeneration = admissionLatch.generation)
  /\ (writerLatch.active => writerAckGeneration < writerLatch.generation)
  /\ (~writerLatch.active => writerAckGeneration = writerLatch.generation)
  /\ (lastPressureAck.accepted =>
        lastPressureAck.requestedGeneration =
          lastPressureAck.liveGeneration)
  /\ (lastPressureAck.requestedGeneration #
        lastPressureAck.liveGeneration => ~lastPressureAck.accepted)

NoPressureCreatedRelease ==
  /\ ~admissionLatch.active
  /\ ~writerLatch.active
  /\ admissionAckGeneration = 0
  /\ writerAckGeneration = 0

CapacityWakeMatchesReclaim ==
  /\ capacityWake.observedGeneration <= capacityWake.generation
  /\ (capacityWake.phase = "Waiting" =>
        \/ capacityWake.observedGeneration < capacityWake.generation
        \/ submitted # <<>>
        \/ completed # <<>>)

TerminalFenceSafety ==
  /\ (~shutdownRequested => terminalFence = 0)
  /\ (shutdownRequested => terminalFence = nextSource - 1)
  /\ (~shutdownComplete \/
        /\ terminalAck
        /\ reclaimed = terminalFence)

Safety ==
  /\ TypeOK
  /\ BoundedStores
  /\ FifoSourceOrder
  /\ DeferredOwnerIsBoundedValue
  /\ DeferredCommandsExactlyOnce
  /\ DeferredIdentityAndEffectBoundary
  /\ OnlyUnaffectedSuccessorRestores
  /\ WriterOwnsDeferredPublication
  /\ ExactSuccessorBeatsPressure
  /\ ReleaseOrControlDrainsBeforeEffects
  /\ OneTentativePrefix
  /\ CompatibleSuffixStaysReady
  /\ NoYoungerThanEarliestFence
  /\ RollbackBeforeEffectsRestoresExactPrefix
  /\ ReleaseQueueOrdered
  /\ AckAfterRequiredActionAndFenceCoverage
  /\ NoCompletionBeforeSubmit
  /\ NoStalePressureAck
  /\ NoPressureCreatedRelease
  /\ CapacityWakeMatchesReclaim
  /\ TerminalFenceSafety

ShutdownProgress == shutdownRequested ~> shutdownComplete

OrdinaryReleaseProgress ==
  \A generation \in 1 .. MaxReleaseGeneration :
    (releaseGeneration >= generation) ~>
      (ordinaryAckGeneration >= generation)

StartupCapacityWakeProgress ==
  (capacityWake.phase = "Waiting") ~>
    (capacityWake.phase \in {"Woken", "DirectStarted"})

StartupDirectProgress ==
  (capacityWake.phase = "Woken") ~>
    (capacityWake.phase = "DirectStarted")

DeferredWriterPublicationProgress ==
  (deferredSuffix.current # 0 /\
   deferredSuffix.writingSuccessor =
     deferredSuffix.expectedSuccessor) ~>
    (deferredSuffix.writingSuccessor = 0)

DeferredExactSuccessorProgress ==
  (deferredSuffix.phase = "SuccessorTentative" /\
   deferredSuffix.drainReason = "Pressure" /\
   deferredSuffix.successorGeneration =
     deferredSuffix.liveSuccessorGeneration) ~>
    (deferredSuffix.phase \in {"JoinEffectful", "Empty"})

DeferredDrainProgress ==
  (deferredSuffix.phase \in {"Held", "SuccessorTentative"} /\
   (ReleasePending \/
    deferredSuffix.drainReason \in
      {"Release", "Control", "Initializer", "Stop", "Loss",
       "WriterLoss", "Headroom", "Lease"})) ~>
    (deferredSuffix.phase = "Empty")

AdmissionPressureProgress ==
  \A generation \in 1 .. MaxPressureGeneration :
    (admissionLatch.active /\ admissionLatch.generation = generation) ~>
      (admissionAckGeneration >= generation)

WriterPressureProgress ==
  \A generation \in 1 .. MaxPressureGeneration :
    (writerLatch.active /\ writerLatch.generation = generation) ~>
      (writerAckGeneration >= generation)

====
