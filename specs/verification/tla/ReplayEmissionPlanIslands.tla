---- MODULE ReplayEmissionPlanIslands ----
(***************************************************************************)
(* R-BACK-2.102 lease-span replay of one raw.                              *)
(*                                                                         *)
(* A raw is partitioned into an ordered sequence of executable spans. Each  *)
(* maximal run of direct islands and coordinator locators owns one          *)
(* final-slot lease; an ordered control or a compatibility range is a cut   *)
(* that executes through the ordinary sink at its exact serial position.    *)
(*                                                                         *)
(* The smallest raw that distinguishes the design is three spans:           *)
(*                                                                         *)
(*   span 0 : direct, lease ordinal 0                                      *)
(*   span 1 : separator (ordered control or compatibility range), ordinary  *)
(*   span 2 : direct, lease ordinal 1                                      *)
(*                                                                         *)
(* with a failure admitted in span 2. That shape carries all three          *)
(* obligations the whole-raw transaction never had:                         *)
(*                                                                         *)
(*  1. Span identity. Every span of one raw presents the SAME closed        *)
(*     producer interval, so the cross-raw +1 adjacency rule cannot         *)
(*     describe them and must not be relaxed to try -- it is what keeps a   *)
(*     *different* raw from appending onto a populated slot. A separate     *)
(*     witness (which raw is extending this slot, how far its span sequence *)
(*     has got, whether it settled, and its exact raw-local interval) admits*)
(*     only the immediate successor span of the exact active raw. The slot  *)
(*     aggregate may already include an older adjacent raw and is never the *)
(*     same-raw witness.                                                     *)
(*                                                                         *)
(*  2. The separator cut. A whole-raw transaction could hand a failed raw   *)
(*     back to Legacy because nothing of it had executed. Once a separator  *)
(*     has executed, that retry would replay an already-executed prefix, so *)
(*     a post-separator failure must be a typed fail-stop.                  *)
(*                                                                         *)
(*  3. Run closure. A DrawRunCommandRecord must not span a cut, or the      *)
(*     draws either side of an executed separator merge into one run and    *)
(*     the separator's effect lands in the wrong place.                     *)
(*                                                                         *)
(* Each obligation is guarded by a discipline constant so a deliberate      *)
(* regression configuration can delete exactly one of them and be checked   *)
(* as an expected counterexample.                                           *)
(*                                                                         *)
(* Out of scope, as always: this proves nothing about Metal driver          *)
(* behaviour, shader ABI bytes, resource contents, or final pixels.         *)
(***************************************************************************)

EXTENDS Naturals, TLC

CONSTANTS
  \* "Enforced" admits only the immediate successor span of the exact active
  \* raw, before settlement. "Removed" drops the witness check entirely.
  SpanIdentityDiscipline,
  \* "Enforced" stores the active raw's exact interval separately from the
  \* populated slot aggregate. "Removed" recreates aggregate-as-witness.
  RawLocalWitnessDiscipline,
  \* "Enforced" turns a post-separator failure into a fail-stop. "Removed"
  \* keeps the whole-raw Legacy retry that was sound only while a raw was
  \* indivisible.
  SeparatorCutDiscipline,
  \* "Enforced" closes the open draw run at every span cut. "Removed" lets a
  \* run stay open across the separator.
  RunClosureDiscipline

Spans == 0..2
DirectSpans == {0, 2}
SeparatorSpan == 1
\* The lease ordinal of a direct span. Ordinary spans consume no ordinal, so
\* lease ordinals stay densely adjacent across an interleaved separator.
LeaseOrdinal(s) == IF s = 0 THEN 0 ELSE 1

Stages == {"Init", "Span0Begun", "Span0Committed", "SeparatorDone",
           "Span2Begun", "Done", "FailStopped", "LegacyWholeRaw"}
Dispositions == {"Unset", "Direct", "Legacy", "FailStop"}
Intervals == {"None", "PreviousRaw", "ActiveRaw", "SlotAggregate"}

VARIABLES stage, disposition, witnessRaw, witnessSpan, witnessSettled,
          slotInterval, witnessInterval,
          emitted, separatorExecuted, runOpen, runStraddledCut,
          effectsStarted, span2Fails

vars == <<stage, disposition, witnessRaw, witnessSpan, witnessSettled,
  slotInterval, witnessInterval, emitted, separatorExecuted, runOpen,
  runStraddledCut, effectsStarted, span2Fails>>

(* The production predicate, `compatibilitySpanAdmission` in
   src/dxmt9/dxmt9_cpu_ready_tape.hpp. A same-raw span is admitted only when
   the witness names this raw, has not settled, and the presented ordinal is
   the immediate successor of the last committed one. *)
AdmitsLeaseOrdinal(n) ==
  /\ witnessRaw = 1
  /\ ~witnessSettled
  /\ witnessInterval = "ActiveRaw"
  /\ n = witnessSpan + 1

\* A span whose witness is inactive takes the unchanged cross-raw path.
AdmitsFirstSpan == witnessRaw = 0

SpanIdentityRelaxed == SpanIdentityDiscipline = "Removed"

Init ==
  /\ stage = "Init"
  /\ disposition = "Unset"
  /\ witnessRaw = 0
  /\ witnessSpan = 0
  /\ witnessSettled = FALSE
  \* The raw begins on a populated slot whose aggregate already owns one
  \* adjacent predecessor, but there is no active span sequence yet.
  /\ slotInterval = "PreviousRaw"
  /\ witnessInterval = "None"
  /\ emitted = [s \in Spans |-> 0]
  /\ separatorExecuted = FALSE
  /\ runOpen = FALSE
  /\ runStraddledCut = FALSE
  /\ effectsStarted = FALSE
  /\ span2Fails \in BOOLEAN

(* Span 0 opens on a slot no raw is currently extending, so it is the
   ordinary cross-raw admission. Its first draw opens a run. *)
BeginSpan0 ==
  /\ stage = "Init"
  /\ AdmitsFirstSpan
  /\ stage' = "Span0Begun"
  /\ runOpen' = TRUE
  /\ UNCHANGED <<disposition, witnessRaw, witnessSpan, witnessSettled,
      slotInterval, witnessInterval, emitted, separatorExecuted,
      runStraddledCut, effectsStarted, span2Fails>>

(* Commit installs the witness and, with the discipline enforced, closes the
   open run so the cut cannot be straddled. *)
CommitSpan0 ==
  /\ stage = "Span0Begun"
  /\ stage' = "Span0Committed"
  /\ disposition' = "Direct"
  /\ witnessRaw' = 1
  /\ witnessSpan' = LeaseOrdinal(0)
  /\ witnessSettled' = FALSE
  /\ slotInterval' = "SlotAggregate"
  /\ witnessInterval' = IF RawLocalWitnessDiscipline = "Enforced"
       THEN "ActiveRaw" ELSE "SlotAggregate"
  /\ emitted' = [emitted EXCEPT ![0] = @ + 1]
  /\ effectsStarted' = TRUE
  /\ runOpen' = IF RunClosureDiscipline = "Enforced" THEN FALSE ELSE runOpen
  /\ UNCHANGED <<separatorExecuted, runStraddledCut, span2Fails>>

ExecuteSeparator ==
  /\ stage = "Span0Committed"
  /\ stage' = "SeparatorDone"
  /\ separatorExecuted' = TRUE
  /\ emitted' = [emitted EXCEPT ![SeparatorSpan] = @ + 1]
  \* A run still open when the cut executes is one run record straddling the
  \* separator, which is exactly the shape the explicit closure prevents.
  /\ runStraddledCut' = (runStraddledCut \/ runOpen)
  /\ runOpen' = FALSE
  /\ UNCHANGED <<disposition, witnessRaw, witnessSpan, witnessSettled,
      slotInterval, witnessInterval, effectsStarted, span2Fails>>

BeginSpan2 ==
  /\ stage = "SeparatorDone"
  /\ \/ SpanIdentityRelaxed
     \/ AdmitsLeaseOrdinal(LeaseOrdinal(2))
  /\ stage' = "Span2Begun"
  /\ runOpen' = TRUE
  /\ UNCHANGED <<disposition, witnessRaw, witnessSpan, witnessSettled,
      slotInterval, witnessInterval, emitted, separatorExecuted,
      runStraddledCut, effectsStarted, span2Fails>>

CommitSpan2 ==
  /\ stage = "Span2Begun"
  /\ ~span2Fails
  /\ stage' = "Done"
  /\ witnessSpan' = LeaseOrdinal(2)
  /\ witnessSettled' = TRUE
  /\ emitted' = [emitted EXCEPT ![2] = @ + 1]
  /\ runOpen' = IF RunClosureDiscipline = "Enforced" THEN FALSE ELSE runOpen
  /\ UNCHANGED <<disposition, witnessRaw, separatorExecuted, runStraddledCut,
      slotInterval, witnessInterval, effectsStarted, span2Fails>>

(* The whole point of the cut. Span 2's own destination is still private and
   rolls back, but the raw is jointly owned the moment the separator ran, so
   the only sound outcome is a typed fail-stop. Deleting the discipline
   restores the whole-raw Legacy retry, which re-emits span 0 and the
   separator a second time. *)
FailSpan2 ==
  /\ stage = "Span2Begun"
  /\ span2Fails
  /\ IF SeparatorCutDiscipline = "Enforced"
       THEN /\ stage' = "FailStopped"
            /\ disposition' = "FailStop"
            /\ UNCHANGED emitted
       ELSE /\ stage' = "LegacyWholeRaw"
            /\ disposition' = "Legacy"
            /\ emitted' = [s \in Spans |-> emitted[s] + 1]
  /\ UNCHANGED <<witnessRaw, witnessSpan, witnessSettled, slotInterval,
      witnessInterval, separatorExecuted, runOpen, runStraddledCut,
      effectsStarted, span2Fails>>

(* A duplicate span: the producer re-presents an ordinal the witness already
   committed. Enforced, the witness refuses it outright. *)
BeginDuplicateSpan0 ==
  /\ stage = "Span0Committed"
  /\ \/ SpanIdentityRelaxed
     \/ AdmitsLeaseOrdinal(LeaseOrdinal(0))
  /\ emitted' = [emitted EXCEPT ![0] = @ + 1]
  /\ UNCHANGED <<stage, disposition, witnessRaw, witnessSpan, witnessSettled,
      slotInterval, witnessInterval, separatorExecuted, runOpen,
      runStraddledCut, effectsStarted, span2Fails>>

(* A post-settlement span: the raw already published its final span. *)
BeginAfterSettled ==
  /\ stage = "Done"
  /\ \/ SpanIdentityRelaxed
     \/ AdmitsLeaseOrdinal(LeaseOrdinal(2))
  /\ emitted' = [emitted EXCEPT ![2] = @ + 1]
  /\ UNCHANGED <<stage, disposition, witnessRaw, witnessSpan, witnessSettled,
      slotInterval, witnessInterval, separatorExecuted, runOpen,
      runStraddledCut, effectsStarted, span2Fails>>

Next ==
  \/ BeginSpan0 \/ CommitSpan0 \/ ExecuteSeparator \/ BeginSpan2
  \/ CommitSpan2 \/ FailSpan2 \/ BeginDuplicateSpan0 \/ BeginAfterSettled

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ stage \in Stages
  /\ disposition \in Dispositions
  /\ witnessRaw \in {0, 1}
  /\ witnessSpan \in 0..2
  /\ witnessSettled \in BOOLEAN
  /\ slotInterval \in Intervals
  /\ witnessInterval \in Intervals
  /\ emitted \in [Spans -> 0..3]
  /\ separatorExecuted \in BOOLEAN
  /\ runOpen \in BOOLEAN
  /\ runStraddledCut \in BOOLEAN
  /\ effectsStarted \in BOOLEAN
  /\ span2Fails \in BOOLEAN

(* Every record of the raw is emitted at most once. This is what the whole-raw
   Legacy retry breaks the moment a raw is divisible. *)
EachRecordEmittedOnce == \A s \in Spans : emitted[s] <= 1

(* Once any separator has executed, the raw can never be handed back to
   Legacy: its prefix is already owned by the queue. *)
NoLegacyRetryAfterSeparator ==
  separatorExecuted => disposition # "Legacy"

(* A DrawRunCommandRecord may not straddle a cut. `Draw, Draw, <cut>, Draw`
   must produce two run records, never one. A run legitimately reopens after
   the cut, so the property is about the cut itself, not about a later run. *)
RunClosedAcrossSeparator == ~runStraddledCut

(* A direct span other than the raw's first may only begin when the witness
   names this exact raw, has not settled, and its ordinal is the immediate
   successor of the last committed one. *)
SpanAdmissionWitnessed ==
  stage \in {"Span2Begun", "Done"} =>
    /\ witnessRaw = 1
    /\ witnessInterval = "ActiveRaw"
    /\ separatorExecuted

(* A populated slot aggregates the predecessor and active raw, while the
   witness retains only the active raw's exact interval. Using the aggregate
   as the witness recreates the positive-headroom GT1 rejection after the
   separator effect. *)
RawLocalWitnessSeparated ==
  stage \in {"Span0Committed", "SeparatorDone", "Span2Begun", "Done"} =>
    /\ slotInterval = "SlotAggregate"
    /\ witnessInterval = "ActiveRaw"

(* Nothing may extend a raw after its final span has committed. *)
NoPostSettlementExtension ==
  witnessSettled => stage \in {"Done", "FailStopped"}

(* A fail-stop is only reachable after effects, and never carries a Legacy
   disposition. *)
FailStopIsTerminal ==
  stage = "FailStopped" => effectsStarted /\ disposition = "FailStop"

====
