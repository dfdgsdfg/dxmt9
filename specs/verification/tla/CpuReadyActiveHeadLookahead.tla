---- MODULE CpuReadyActiveHeadLookahead ----
(***************************************************************************
 * Active-seeded CPU-ready retained-head refinement.
 *
 * Source 1 is already represented by an ActiveRenderComplete session.
 * An already-adopted immediate replay-FIFO raw lets publication make source 2
 * Ready while exposing a generation-stamped storage-free intent for source 3.
 * An empty FIFO never creates the intent and source 2 proceeds directly.
 * Source 3 may still have no payload/storage reservation when the coordinator
 * moves source 2 to TentativeRepresented; a non-source disposition cancels
 * the intent before its replay effect.
 * The coordinator waits without the queue mutex and restores source 2 before
 * either the ordinary active-seeded two-source planner or exact serial
 * fallback runs.
 *
 * DropActiveSeed is a deliberate historical R15 mutation: the restored
 * window reaches a seedless planner.  The production configuration disables
 * it; the companion configuration must violate ActiveSeedPreserved.
 *************************************************************************)

EXTENDS Naturals, Sequences, TLC

CONSTANT DropActiveSeed

Phases == {"Publishing", "Ready", "Held", "Restored", "Encoded", "Submitted"}
WriterStates == {"Absent", "Writing", "Ready", "Gone"}
DrainReasons ==
  {"None", "Release", "ProducerWait", "Initializer", "Stop", "Pressure"}
SemanticDrainReasons ==
  {"Release", "ProducerWait", "Initializer", "Stop"}
Seeds == {"None", "ActiveRenderComplete"}
NextRawStates == {"Absent", "Adopted"}
NextRawDispositions == {"Direct", "NonSource"}

VARIABLES phase, writer, drain, readyOrder, encodedOrder, headEffects,
          successorEffects, plannerSeed, fallback, intentGeneration,
          nextRaw, nextDisposition

vars == <<phase, writer, drain, readyOrder, encodedOrder, headEffects,
          successorEffects, plannerSeed, fallback, intentGeneration,
          nextRaw, nextDisposition>>

Init ==
  /\ phase = "Publishing"
  /\ writer = "Absent"
  /\ drain = "None"
  /\ readyOrder = <<>>
  /\ encodedOrder = <<>>
  /\ headEffects = 0
  /\ successorEffects = 0
  /\ plannerSeed = "ActiveRenderComplete"
  /\ fallback = FALSE
  /\ intentGeneration = 0
  /\ nextRaw \in NextRawStates
  /\ nextDisposition \in NextRawDispositions

PublishCurrent ==
  /\ phase = "Publishing"
  /\ phase' = "Ready"
  /\ readyOrder' = <<2>>
  /\ intentGeneration' = IF nextRaw = "Adopted" THEN 1 ELSE 0
  /\ UNCHANGED <<writer, drain, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  nextRaw, nextDisposition>>

Hold ==
  /\ phase = "Ready"
  /\ writer \in {"Absent", "Writing"}
  /\ drain = "None"
  /\ intentGeneration = 1
  /\ readyOrder = <<2>>
  /\ phase' = "Held"
  /\ readyOrder' = <<>>
  /\ UNCHANGED <<writer, drain, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

EncodeUnheldSingle ==
  /\ phase = "Ready"
  /\ intentGeneration = 0
  /\ readyOrder = <<2>>
  /\ phase' = "Encoded"
  /\ readyOrder' = <<>>
  /\ encodedOrder' = <<2>>
  /\ headEffects' = 1
  /\ UNCHANGED <<writer, drain, successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

BeginSuccessorWriting ==
  /\ phase \in {"Ready", "Held"}
  /\ nextRaw = "Adopted"
  /\ nextDisposition = "Direct"
  /\ writer = "Absent"
  /\ writer' = "Writing"
  /\ UNCHANGED <<phase, drain, readyOrder, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

PublishExactSuccessor ==
  /\ phase = "Held"
  /\ nextDisposition = "Direct"
  /\ writer \in {"Absent", "Writing"}
  /\ writer' = "Ready"
  /\ UNCHANGED <<phase, drain, readyOrder, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

CancelNonSourceIntent ==
  /\ phase = "Held"
  /\ nextRaw = "Adopted"
  /\ nextDisposition = "NonSource"
  /\ phase' = "Restored"
  /\ writer' = "Gone"
  /\ readyOrder' = <<2>>
  /\ fallback' = TRUE
  /\ intentGeneration' = 0
  /\ UNCHANGED <<drain, encodedOrder, headEffects, successorEffects,
                  plannerSeed, nextRaw, nextDisposition>>

LoseWriter ==
  /\ phase = "Held"
  /\ writer \in {"Absent", "Writing"}
  /\ writer' = "Gone"
  /\ UNCHANGED <<phase, drain, readyOrder, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

PostDrain(reason) ==
  /\ phase = "Held"
  /\ drain = "None"
  /\ reason \in DrainReasons \ {"None"}
  /\ drain' = reason
  /\ UNCHANGED <<phase, writer, readyOrder, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

PostAnyDrain ==
  \E reason \in DrainReasons \ {"None"} : PostDrain(reason)

RestoreForSuccessor ==
  /\ phase = "Held"
  /\ writer = "Ready"
  /\ drain \in {"None", "Pressure"}
  /\ phase' = "Restored"
  /\ readyOrder' = <<2, 3>>
  /\ plannerSeed' =
       IF DropActiveSeed THEN "None" ELSE "ActiveRenderComplete"
  /\ UNCHANGED <<writer, drain, encodedOrder, headEffects,
                  successorEffects, fallback, intentGeneration,
                  nextRaw, nextDisposition>>

RestoreForFallback ==
  /\ phase = "Held"
  /\ \/ drain \in SemanticDrainReasons
     \/ writer = "Gone"
     \/ /\ drain = "Pressure"
        /\ writer # "Ready"
  /\ phase' = "Restored"
  /\ readyOrder' = <<2>>
  /\ fallback' = TRUE
  /\ UNCHANGED <<writer, drain, encodedOrder, headEffects,
                  successorEffects, plannerSeed, intentGeneration,
                  nextRaw, nextDisposition>>

EncodePlannedWindow ==
  /\ phase = "Restored"
  /\ ~fallback
  /\ writer = "Ready"
  /\ readyOrder = <<2, 3>>
  /\ plannerSeed = "ActiveRenderComplete"
  /\ phase' = "Encoded"
  /\ readyOrder' = <<>>
  /\ encodedOrder' = <<2, 3>>
  /\ headEffects' = 1
  /\ successorEffects' = 1
  /\ UNCHANGED <<writer, drain, plannerSeed, fallback>>
  /\ UNCHANGED <<intentGeneration, nextRaw, nextDisposition>>

EncodeFallbackHead ==
  /\ phase = "Restored"
  /\ fallback
  /\ readyOrder = <<2>>
  /\ phase' = "Encoded"
  /\ readyOrder' = <<>>
  /\ encodedOrder' = <<2>>
  /\ headEffects' = 1
  /\ UNCHANGED <<writer, drain, successorEffects, plannerSeed, fallback>>
  /\ UNCHANGED <<intentGeneration, nextRaw, nextDisposition>>

Submit ==
  /\ phase = "Encoded"
  /\ phase' = "Submitted"
  /\ UNCHANGED <<writer, drain, readyOrder, encodedOrder, headEffects,
                  successorEffects, plannerSeed, fallback,
                  intentGeneration, nextRaw, nextDisposition>>

Next ==
  \/ PublishCurrent
  \/ BeginSuccessorWriting
  \/ Hold
  \/ EncodeUnheldSingle
  \/ PublishExactSuccessor
  \/ CancelNonSourceIntent
  \/ LoseWriter
  \/ PostAnyDrain
  \/ RestoreForSuccessor
  \/ RestoreForFallback
  \/ EncodePlannedWindow
  \/ EncodeFallbackHead
  \/ Submit

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(BeginSuccessorWriting)
  /\ WF_vars(PublishExactSuccessor)
  /\ WF_vars(CancelNonSourceIntent)
  /\ WF_vars(RestoreForSuccessor)
  /\ WF_vars(RestoreForFallback)
  /\ WF_vars(EncodeUnheldSingle)
  /\ WF_vars(EncodePlannedWindow)
  /\ WF_vars(EncodeFallbackHead)
  /\ WF_vars(Submit)

TypeOK ==
  /\ phase \in Phases
  /\ writer \in WriterStates
  /\ drain \in DrainReasons
  /\ readyOrder \in Seq({2, 3})
  /\ encodedOrder \in Seq({2, 3})
  /\ headEffects \in 0..1
  /\ successorEffects \in 0..1
  /\ plannerSeed \in Seeds
  /\ fallback \in BOOLEAN
  /\ intentGeneration \in 0..1
  /\ nextRaw \in NextRawStates
  /\ nextDisposition \in NextRawDispositions

IntentRequiresAdoptedRaw ==
  intentGeneration = 1 => nextRaw = "Adopted"

EmptyFifoNeverPromises ==
  nextRaw = "Absent" => intentGeneration = 0

HeldOwnsNoNewEffects ==
  phase = "Held" =>
    /\ readyOrder = <<>>
    /\ encodedOrder = <<>>
    /\ headEffects = 0
    /\ successorEffects = 0

RestorePreservesFifo ==
  phase = "Restored" =>
    IF fallback THEN readyOrder = <<2>> ELSE readyOrder = <<2, 3>>

ActiveSeedPreserved ==
  (~fallback /\ phase \in {"Restored", "Encoded", "Submitted"}) =>
    plannerSeed = "ActiveRenderComplete"

ExactlyOnce ==
  /\ headEffects <= 1
  /\ successorEffects <= 1
  /\ (successorEffects = 1 => headEffects = 1)
  /\ (fallback => successorEffects = 0)

Safety ==
  /\ TypeOK
  /\ HeldOwnsNoNewEffects
  /\ IntentRequiresAdoptedRaw
  /\ EmptyFifoNeverPromises
  /\ RestorePreservesFifo
  /\ ActiveSeedPreserved
  /\ ExactlyOnce

HeldExitProgress ==
  (phase = "Held" /\ (writer \in {"Ready", "Gone"} \/ drain # "None"))
    ~> phase # "Held"

RestoredProgress ==
  phase = "Restored" ~> phase = "Submitted"

EmptyFifoProgress ==
  (nextRaw = "Absent" /\ phase = "Ready") ~> phase = "Submitted"

AdoptedRawProgress ==
  (nextRaw = "Adopted" /\ phase = "Held") ~> phase # "Held"

=============================================================================
