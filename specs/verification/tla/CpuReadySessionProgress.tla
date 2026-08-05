---- MODULE CpuReadySessionProgress ----
(***************************************************************************
 * Bounded CPU-ready Tape / EncodeSession progress refinement.
 *
 * This model deliberately abstracts payload bytes, Metal commands, and the
 * individual ordered-control reasons into two required-action classes:
 * Submit and PassClose.  It retains the scheduling facts that matter here:
 * a bounded FIFO, one exact pre-effect tentative prefix, queue/latch fences,
 * session effects and acknowledgement, ordered completion/reclaim, and a
 * terminal shutdown fence. The terminal action is a forward-looking model
 * obligation: production still drains shutdown directly and does not yet call
 * SessionReleaseState::requestTerminal.
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
  lastRollbackPreEffect

vars ==
  <<nextSource, ready, tentative, tentativeBaseSuffix,
    tentativeValidation, validationFailedOnce, session, passOpen, submitted,
    completed, reclaimed, effectsThrough, passClosedThrough, releaseQ,
    releaseGeneration, ordinaryAckGeneration, ackedSubmitFences,
    ackedPassCloseFences, admissionLatch, writerLatch,
    admissionAckGeneration, writerAckGeneration, lastPressureAck,
    shutdownRequested, terminalFence, terminalAck, shutdownComplete,
    lastRollbackPrefix, lastRollbackSuffix, lastRollbackReady,
    lastRollbackPreEffect>>

SeqSet(seq) == {seq[i] : i \in DOMAIN seq}

Prefix(seq, count) == [i \in 1 .. count |-> seq[i]]
Suffix(seq, count) ==
  [i \in 1 .. (Len(seq) - count) |-> seq[count + i]]

IsPrefix(prefix, seq) ==
  /\ Len(prefix) <= Len(seq)
  /\ \A i \in DOMAIN prefix : prefix[i] = seq[i]

IsConsecutive(seq) ==
  \A i \in DOMAIN seq : i < Len(seq) => seq[i + 1] = seq[i] + 1

LiveOrder == completed \o submitted \o session \o tentative \o ready
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

Init ==
  /\ nextSource = 1
  /\ ready = <<>>
  /\ tentative = <<>>
  /\ tentativeBaseSuffix = <<>>
  /\ tentativeValidation = "Pass"
  /\ validationFailedOnce = {}
  /\ session = <<>>
  /\ passOpen = FALSE
  /\ submitted = <<>>
  /\ completed = <<>>
  /\ reclaimed = 0
  /\ effectsThrough = 0
  /\ passClosedThrough = 0
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
                  lastRollbackPreEffect>>

ReservePrefix(count, outcome) ==
  /\ tentative = <<>>
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
                  lastRollbackPreEffect>>

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
  /\ UNCHANGED <<nextSource, ready, validationFailedOnce, submitted,
                  completed, reclaimed, passClosedThrough, releaseQ,
                  releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect>>

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
                  terminalAck, shutdownComplete>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

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
                  lastRollbackPreEffect>>

ReclaimCompleted ==
  /\ completed # <<>>
  /\ Head(completed) = reclaimed + 1
  /\ reclaimed' = Head(completed)
  /\ completed' = Tail(completed)
  /\ UNCHANGED <<nextSource, ready, tentative, tentativeBaseSuffix,
                  tentativeValidation, validationFailedOnce, session,
                  passOpen, submitted, effectsThrough, passClosedThrough,
                  releaseQ, releaseGeneration, ordinaryAckGeneration,
                  ackedSubmitFences, ackedPassCloseFences, admissionLatch,
                  writerLatch, admissionAckGeneration, writerAckGeneration,
                  lastPressureAck, shutdownRequested, terminalFence,
                  terminalAck, shutdownComplete, lastRollbackPrefix,
                  lastRollbackSuffix, lastRollbackReady,
                  lastRollbackPreEffect>>

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
                  lastRollbackReady, lastRollbackPreEffect>>

Next ==
  \/ Publish
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
  /\ WF_vars(FinishShutdown)

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
  /\ OneTentativePrefix
  /\ CompatibleSuffixStaysReady
  /\ NoYoungerThanEarliestFence
  /\ RollbackBeforeEffectsRestoresExactPrefix
  /\ ReleaseQueueOrdered
  /\ AckAfterRequiredActionAndFenceCoverage
  /\ NoCompletionBeforeSubmit
  /\ NoStalePressureAck
  /\ NoPressureCreatedRelease
  /\ TerminalFenceSafety

ShutdownProgress == shutdownRequested ~> shutdownComplete

OrdinaryReleaseProgress ==
  \A generation \in 1 .. MaxReleaseGeneration :
    (releaseGeneration >= generation) ~>
      (ordinaryAckGeneration >= generation)

AdmissionPressureProgress ==
  \A generation \in 1 .. MaxPressureGeneration :
    (admissionLatch.active /\ admissionLatch.generation = generation) ~>
      (admissionAckGeneration >= generation)

WriterPressureProgress ==
  \A generation \in 1 .. MaxPressureGeneration :
    (writerLatch.active /\ writerLatch.generation = generation) ~>
      (writerAckGeneration >= generation)

====
