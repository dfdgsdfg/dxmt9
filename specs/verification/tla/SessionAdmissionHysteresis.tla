---- MODULE SessionAdmissionHysteresis ----
(* R-BACK-2.65: page admission latch x open session x deferred Clear.
 * The seed is a full Arena: an immediately retireable prefix followed by a
 * Clear-pinned suffix. FIFO representation, post-encode retirement, fixed-cap
 * submission, and eventual submitted-payload reclaim are abstract interfaces.
 * No timer/pressure action submits work. Byte contents, actual GPU completion,
 * wrap allocation, and other capacity dimensions belong to their own models.
 * Native productionLoopReopensLatchedPagesWithPendingClear binds the real
 * coordinator, Tape latch, retirement predicates and FIFO to this reduction.
 *)
EXTENDS Naturals, Sequences, TLC

CONSTANT IgnoreLowWater

HighWater == 8
LowWater == 4
SuccessorHeadroom == 3
(* Shared algebra: render::sessionResidencyLimitForHysteresis. *)
SessionLimit == IF IgnoreLowWater THEN HighWater - SuccessorHeadroom
                ELSE IF LowWater < HighWater - SuccessorHeadroom
                     THEN LowWater ELSE HighWater - SuccessorHeadroom
Pages(s) == IF s \in {1, 3, 4} THEN 2 ELSE 1
RECURSIVE Total(_)
Total(ss) == IF ss = <<>> THEN 0 ELSE Pages(Head(ss)) + Total(Tail(ss))

VARIABLES ready, session, held, submitted, completed, encoded,
          closed, writerPublished, cuts
vars == <<ready, session, held, submitted, completed, encoded,
          closed, writerPublished, cuts>>
ResidentPages == Total(ready) + Total(held) + Total(submitted)

Init ==
  /\ ready = <<1, 2, 3, 4, 5>>
  /\ session = <<>> /\ held = <<>> /\ submitted = <<>>
  /\ completed = <<>> /\ encoded = <<>>
  /\ closed = TRUE /\ writerPublished = FALSE /\ cuts = <<>>

EncodeHead ==
  /\ ready # <<>>
  /\ submitted = <<>>
  /\ Total(held) + Pages(Head(ready)) <= SessionLimit
  /\ LET s == Head(ready)
         retired == s <= 2
     IN /\ ready' = Tail(ready)
        /\ session' = Append(session, s)
        /\ held' = IF retired THEN held ELSE Append(held, s)
        /\ encoded' = Append(encoded, s)
        /\ closed' = (closed /\
             ResidentPages - (IF retired THEN Pages(s) ELSE 0) > LowWater)
  /\ UNCHANGED <<submitted, completed, writerPublished, cuts>>

SubmitCap ==
  /\ ready # <<>> /\ session # <<>> /\ submitted = <<>>
  /\ Total(held) + Pages(Head(ready)) > SessionLimit
  /\ submitted' = held
  /\ session' = <<>> /\ held' = <<>>
  /\ cuts' = Append(cuts, "Cap")
  /\ UNCHANGED <<ready, completed, encoded, closed, writerPublished>>

(* The final writer is a semantic boundary such as Present. *)
SubmitTerminal ==
  /\ session # <<>> /\ session[Len(session)] = 6
  /\ submitted = <<>>
  /\ submitted' = held /\ session' = <<>> /\ held' = <<>>
  /\ cuts' = Append(cuts, "Semantic")
  /\ UNCHANGED <<ready, completed, encoded, closed, writerPublished>>

ReclaimSubmitted ==
  /\ submitted # <<>>
  /\ completed' = completed \o submitted
  /\ submitted' = <<>>
  /\ closed' = (closed /\ ResidentPages - Total(submitted) > LowWater)
  /\ UNCHANGED <<ready, session, held, encoded, writerPublished, cuts>>

PublishWriter ==
  /\ ~writerPublished /\ ~closed
  /\ ResidentPages + Pages(6) <= HighWater
  /\ ready' = Append(ready, 6) /\ writerPublished' = TRUE
  /\ closed' = (ResidentPages + Pages(6) >= HighWater)
  /\ UNCHANGED <<session, held, submitted, completed, encoded, cuts>>

Next == EncodeHead \/ SubmitCap \/ ReclaimSubmitted \/
        PublishWriter \/ SubmitTerminal
Spec == Init /\ [][Next]_vars
  /\ WF_vars(EncodeHead) /\ WF_vars(SubmitCap)
  /\ WF_vars(ReclaimSubmitted) /\ WF_vars(PublishWriter)
  /\ WF_vars(SubmitTerminal)

NoClosedEmptySessionCycle ==
  ~(closed /\ ready = <<>> /\ submitted = <<>> /\ ~writerPublished)
Safety ==
  /\ ResidentPages <= HighWater /\ Total(held) <= SessionLimit
  /\ \A i \in DOMAIN encoded : encoded[i] = i
  /\ \A i \in DOMAIN cuts : cuts[i] \in {"Cap", "Semantic"}
  /\ NoClosedEmptySessionCycle
WriterProgress == <>writerPublished
TerminalProgress == <>(6 \in {completed[i] : i \in DOMAIN completed})
====
