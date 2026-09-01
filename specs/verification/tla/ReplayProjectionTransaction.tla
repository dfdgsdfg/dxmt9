(***************************************************************************)
(* Source-wide replay projection transaction.                              *)
(*                                                                         *)
(* The source stream is immutable.  A transaction creates a working state, *)
(* derives one representation-preserving effective stream and exact plan,   *)
(* emits that plan into an uncommitted receipt, and commits persistent state *)
(* only after the complete receipt exists.  This is deliberately bounded to *)
(* the replay/emission receipt boundary; it does not model queue or GPU      *)
(* completion.                                                               *)
(*                                                                         *)
(* The negative configurations are executable counterexamples for the eight  *)
(* failure shapes which must remain impossible in production: state commit  *)
(* before emission, partial publication, optimizer authority without proof, *)
(* optimizer state mutation, stale source generation, ordered-control        *)
(* reordering, dedup attribution loss, and retry after an effect.            *)
***************************************************************************)

---- MODULE ReplayProjectionTransaction ----
EXTENDS Naturals, Sequences, TLC

CONSTANTS
  StateCommitDiscipline,
  PublicationDiscipline,
  OptimizerDiscipline,
  OptimizerProofDiscipline,
  DedupAttributionDiscipline,
  GenerationDiscipline,
  ControlOrderDiscipline,
  RetryDiscipline

InitialPersistentState == 0
ProjectedState == 2
InitialGeneration == 1
StatePath == <<"A", "B", "A">>
StateGenerationPath == <<InitialGeneration, InitialGeneration + 1,
                          InitialGeneration + 2>>

(* State records are intentionally not emitted as independent effects here;
   their final value is part of the working projection.  Draw and ordered
   control entries remain in the effective stream and therefore retain exact
   serial order. *)
SourceStream == <<"DrawA", "Control", "DrawB", "DrawC">>
SourceLength == Len(SourceStream)
Attribution == [source: {"S"}, sequence: {7}, record: 1..4]
SourceAttribution ==
  <<[source |-> "S", sequence |-> 7, record |-> 1],
    [source |-> "S", sequence |-> 7, record |-> 2],
    [source |-> "S", sequence |-> 7, record |-> 3],
    [source |-> "S", sequence |-> 7, record |-> 4]>>

Phases == {"Idle", "Working", "Planned", "Emitting", "Committed",
           "Fallback", "Poisoned"}
Dispositions == {"None", "Committed", "Fallback", "Poisoned"}

VARIABLES
  phase,
  persistentState,
  workingState,
  projectedState,
  persistentIdentity,
  workingIdentity,
  statePath,
  stateGenerationPath,
  optimizerState,
  optimizerProof,
  dedupAttribution,
  persistentGeneration,
  workingGeneration,
  effectiveStream,
  exactPlan,
  emitted,
  emittedAttribution,
  receipt,
  publishedCount,
  effectsStarted,
  retryCount,
  disposition

vars == <<phase, persistentState, workingState, projectedState,
  persistentIdentity, workingIdentity, statePath, stateGenerationPath,
  optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
  workingGeneration, effectiveStream, exactPlan, emitted, emittedAttribution,
  receipt,
  publishedCount, effectsStarted, retryCount, disposition>>

Init ==
  /\ phase = "Idle"
  /\ persistentState = InitialPersistentState
  /\ workingState = InitialPersistentState
  /\ projectedState = ProjectedState
  /\ persistentIdentity = "A"
  /\ workingIdentity = "A"
  /\ statePath = <<"A">>
  /\ stateGenerationPath = <<InitialGeneration>>
  /\ optimizerState = InitialPersistentState
  /\ optimizerProof = TRUE
  /\ dedupAttribution = <<>>
  /\ persistentGeneration = InitialGeneration
  /\ workingGeneration = InitialGeneration
  /\ effectiveStream = <<>>
  /\ exactPlan = <<>>
  /\ emitted = <<>>
  /\ emittedAttribution = <<>>
  /\ receipt = <<>>
  /\ publishedCount = 0
  /\ effectsStarted = FALSE
  /\ retryCount = 0
  /\ disposition = "None"

Begin ==
  /\ phase = "Idle"
  /\ phase' = "Working"
  /\ workingState' = persistentState
  /\ workingGeneration' = persistentGeneration
  /\ workingIdentity' = persistentIdentity
  /\ statePath' = <<persistentIdentity>>
  /\ stateGenerationPath' = <<persistentGeneration>>
  /\ UNCHANGED <<persistentState, projectedState, persistentIdentity,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    effectiveStream, exactPlan, emitted, emittedAttribution, receipt,
    publishedCount,
    effectsStarted, retryCount, disposition>>

(* The source-wide state fold explicitly exercises the representative ABA
   trace.  The payload identity goes A -> B -> A while the generation is
   monotonic 1 -> 2 -> 3; the final A is therefore not an old generation. *)
AdvanceAtoB ==
  /\ phase = "Working"
  /\ statePath = <<"A">>
  /\ stateGenerationPath = <<persistentGeneration>>
  /\ statePath' = <<"A", "B">>
  /\ stateGenerationPath' = <<persistentGeneration,
                                persistentGeneration + 1>>
  /\ workingIdentity' = "B"
  /\ workingGeneration' = persistentGeneration + 1
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, optimizerState, optimizerProof, dedupAttribution,
    persistentGeneration, effectiveStream, exactPlan,
    emitted, emittedAttribution, receipt, publishedCount, effectsStarted,
    retryCount, disposition>>

AdvanceBtoA ==
  /\ phase = "Working"
  /\ statePath = <<"A", "B">>
  /\ stateGenerationPath = <<persistentGeneration,
                                persistentGeneration + 1>>
  /\ statePath' = StatePath
  /\ stateGenerationPath' = StateGenerationPath
  /\ workingIdentity' = "A"
  /\ workingGeneration' = persistentGeneration + 2
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, optimizerState, optimizerProof, dedupAttribution,
    persistentGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, publishedCount, effectsStarted, retryCount,
    disposition>>

(* The source-wide projection is a value operation.  It does not publish a
   slot, mutate persistent state, or create a visible effect. *)
Prepare ==
  /\ phase = "Working"
  /\ statePath = StatePath
  /\ stateGenerationPath = StateGenerationPath
  /\ phase' = "Planned"
  /\ effectiveStream' = SourceStream
  /\ exactPlan' = SourceStream
  /\ workingState' = projectedState
  /\ workingIdentity' = "A"
  /\ statePath' = StatePath
  /\ stateGenerationPath' = StateGenerationPath
  /\ optimizerState' = InitialPersistentState
  /\ optimizerProof' = TRUE
  /\ dedupAttribution' = SourceAttribution
  /\ UNCHANGED <<persistentState, projectedState, persistentIdentity,
    workingGeneration, persistentGeneration,
    emitted, emittedAttribution, receipt, publishedCount, effectsStarted,
    retryCount, disposition>>

StartEmission ==
  /\ phase = "Planned"
  /\ effectiveStream # <<>>
  /\ exactPlan = effectiveStream
  /\ phase' = "Emitting"
  /\ UNCHANGED <<persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted, receipt,
    emittedAttribution, publishedCount, effectsStarted, retryCount,
    disposition>>

EmitNext ==
  /\ phase = "Emitting"
  /\ Len(emitted) < SourceLength
  /\ emitted' = Append(emitted, effectiveStream[Len(emitted) + 1])
  /\ receipt' = Append(receipt, effectiveStream[Len(receipt) + 1])
  /\ emittedAttribution' = Append(emittedAttribution,
      dedupAttribution[Len(emittedAttribution) + 1])
  /\ effectsStarted' = TRUE
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, publishedCount,
    retryCount, disposition>>

(* A complete receipt is the only publication point.  Persistent state and
   generation advance together with the receipt, after all entries emitted. *)
Commit ==
  /\ phase = "Emitting"
  /\ emitted = effectiveStream
  /\ receipt = exactPlan
  /\ phase' = "Committed"
  /\ persistentState' = workingState
  /\ persistentIdentity' = workingIdentity
  /\ persistentGeneration' = workingGeneration
  /\ workingGeneration' = workingGeneration
  /\ publishedCount' = SourceLength
  /\ disposition' = "Committed"
  /\ UNCHANGED <<workingState, projectedState, workingIdentity, statePath,
    stateGenerationPath, optimizerState, optimizerProof, dedupAttribution,
    effectiveStream, exactPlan, emitted, emittedAttribution, receipt,
    effectsStarted, retryCount>>

(* A failure before the first emission has no effect and can abandon the
   transaction without changing persistent state. *)
PreEffectFallback ==
  /\ phase = "Planned"
  /\ ~effectsStarted
  /\ phase' = "Fallback"
  /\ disposition' = "Fallback"
  /\ UNCHANGED <<persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, publishedCount, effectsStarted, retryCount>>

(* Entering a bridge/encoder failure after an emission receipt is an unknown
   effect.  It is terminal and cannot be retried through another route. *)
PostEffectFailStop ==
  /\ phase = "Emitting"
  /\ effectsStarted
  /\ phase' = "Poisoned"
  /\ disposition' = "Poisoned"
  /\ UNCHANGED <<persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, publishedCount, effectsStarted, retryCount>>

(* Negative control: a producer commits its working state as soon as the
   plan is available, before the first receipt entry. *)
EarlyStateCommit ==
  /\ phase = "Planned"
  /\ StateCommitDiscipline = "Early"
  /\ persistentState' = workingState
  /\ persistentGeneration' = persistentGeneration + 1
  /\ phase' = "Planned"
  /\ UNCHANGED <<workingState, projectedState, persistentIdentity,
    workingIdentity, statePath, stateGenerationPath, optimizerState,
    optimizerProof, dedupAttribution, workingGeneration, effectiveStream,
    exactPlan, emitted, emittedAttribution, receipt, publishedCount,
    effectsStarted, retryCount, disposition>>

(* Negative control: one receipt member is published while the transaction is
   still emitting. *)
PublishPartial ==
  /\ phase = "Emitting"
  /\ PublicationDiscipline = "Partial"
  /\ Len(emitted) > 0
  /\ publishedCount' = 1
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, effectsStarted, retryCount, disposition>>

(* Negative control: an optimizer is allowed to mutate the authoritative
   working state without a proof-carrying projection. *)
OptimizerMutatesWorking ==
  /\ phase = "Planned"
  /\ OptimizerDiscipline = "MutateWorking"
  /\ optimizerState' = 99
  /\ workingState' = 99
  /\ UNCHANGED <<phase, persistentState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, publishedCount, effectsStarted, retryCount,
    disposition>>

(* Negative control: a representation-changing optimizer publishes a rewritten
   stream without an authenticated proof that it preserves source semantics. *)
OptimizerWithoutProof ==
  /\ phase = "Planned"
  /\ OptimizerProofDiscipline = "Unproved"
  /\ effectiveStream' = <<"DrawA", "DrawB", "DrawC">>
  /\ exactPlan' = effectiveStream'
  /\ optimizerProof' = FALSE
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, dedupAttribution, persistentGeneration,
    workingGeneration, emitted, emittedAttribution, receipt, publishedCount,
    effectsStarted, retryCount, disposition>>

(* Negative control: a deduplicator drops the source-qualified locator for a
   record while retaining only the payload shape.  This is not a legal logical
   deduplication: source/sequence/record attribution is part of the receipt. *)
DropDedupAttribution ==
  /\ phase = "Planned"
  /\ DedupAttributionDiscipline = "DropLocator"
  /\ dedupAttribution' =
      <<[source |-> "S", sequence |-> 7, record |-> 1],
        [source |-> "S", sequence |-> 7, record |-> 3],
        [source |-> "S", sequence |-> 7, record |-> 4]>>
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, persistentGeneration, workingGeneration,
    effectiveStream, exactPlan, emitted, emittedAttribution, receipt,
    publishedCount, effectsStarted, retryCount, disposition>>

(* Negative control: the working projection is detached from the current
   source generation. *)
UseStaleGeneration ==
  /\ phase \in {"Working", "Planned"}
  /\ GenerationDiscipline = "Stale"
  /\ workingGeneration' = 0
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    effectiveStream, exactPlan, emitted, emittedAttribution, receipt,
    publishedCount, effectsStarted, retryCount, disposition>>

(* Negative control: the ordered-control record is inserted ahead of the
   next draw.  It is deliberately bounded to a single wrong insertion. *)
ReorderOrderedControl ==
  /\ phase = "Emitting"
  /\ ControlOrderDiscipline = "AllowReorder"
  /\ emitted = <<"DrawA">>
  /\ emitted' = Append(emitted, "DrawB")
  /\ receipt' = Append(receipt, "DrawB")
  /\ emittedAttribution' = Append(emittedAttribution,
      [source |-> "S", sequence |-> 7, record |-> 3])
  /\ effectsStarted' = TRUE
  /\ UNCHANGED <<phase, persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, publishedCount,
    retryCount, disposition>>

(* Negative control: once a visible effect exists, the source is sent back to
   the working phase and can be emitted a second time. *)
RetryAfterEffect ==
  /\ phase = "Emitting"
  /\ RetryDiscipline = "AllowAfterEffect"
  /\ effectsStarted
  /\ phase' = "Working"
  /\ retryCount' = retryCount + 1
  /\ UNCHANGED <<persistentState, workingState, projectedState,
    persistentIdentity, workingIdentity, statePath, stateGenerationPath,
    optimizerState, optimizerProof, dedupAttribution, persistentGeneration,
    workingGeneration, effectiveStream, exactPlan, emitted,
    emittedAttribution, receipt, publishedCount, effectsStarted, disposition>>

Next == Begin \/ AdvanceAtoB \/ AdvanceBtoA \/ Prepare \/ StartEmission \/ EmitNext \/ Commit
        \/ PreEffectFallback \/ PostEffectFailStop \/ EarlyStateCommit
        \/ PublishPartial \/ OptimizerMutatesWorking \/ OptimizerWithoutProof
        \/ DropDedupAttribution \/ UseStaleGeneration
        \/ ReorderOrderedControl \/ RetryAfterEffect

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in Phases
  /\ persistentState \in Nat
  /\ workingState \in Nat
  /\ projectedState \in Nat
  /\ persistentIdentity \in {"A", "B"}
  /\ workingIdentity \in {"A", "B"}
  /\ statePath \in Seq({"A", "B"})
  /\ stateGenerationPath \in Seq(Nat)
  /\ optimizerState \in Nat
  /\ optimizerProof \in BOOLEAN
  /\ dedupAttribution \in Seq(Attribution)
  /\ persistentGeneration \in Nat
  /\ workingGeneration \in Nat
  /\ effectiveStream \in Seq({"DrawA", "Control", "DrawB", "DrawC"})
  /\ exactPlan \in Seq({"DrawA", "Control", "DrawB", "DrawC"})
  /\ emitted \in Seq({"DrawA", "Control", "DrawB", "DrawC"})
  /\ emittedAttribution \in Seq(Attribution)
  /\ receipt \in Seq({"DrawA", "Control", "DrawB", "DrawC"})
  /\ publishedCount \in Nat
  /\ effectsStarted \in BOOLEAN
  /\ retryCount \in Nat
  /\ disposition \in Dispositions

ProjectionIsRepresentationPreserving ==
  phase \in {"Planned", "Emitting", "Committed"}
    => /\ effectiveStream = SourceStream
       /\ exactPlan = SourceStream

WorkingStateMatchesProjection ==
  phase \in {"Planned", "Emitting", "Committed"}
    => workingState = projectedState

PersistentStateCommitsOnlyAfterReceipt ==
  phase \in {"Idle", "Working", "Planned", "Emitting", "Fallback", "Poisoned"}
    => persistentState = InitialPersistentState

WorkingGenerationQualified ==
  phase \in {"Working", "Planned", "Emitting"}
    => /\ workingGeneration = persistentGeneration
                              + Len(stateGenerationPath) - 1
       /\ Len(stateGenerationPath) <= Len(StateGenerationPath)
       /\ stateGenerationPath = SubSeq(StateGenerationPath, 1,
                                       Len(stateGenerationPath))

StateGenerationTraceExact ==
  phase \in {"Planned", "Emitting", "Committed"}
    => /\ statePath = StatePath
       /\ stateGenerationPath = StateGenerationPath
       /\ workingIdentity = "A"

DedupAttributionExact ==
  phase \in {"Planned", "Emitting", "Committed"}
    => dedupAttribution = SourceAttribution

EmittedLogicalAttributionExact ==
  phase \in {"Emitting", "Committed", "Poisoned"}
    => emittedAttribution = SubSeq(SourceAttribution, 1,
                                   Len(emittedAttribution))

OptimizerRequiresProof ==
  phase \in {"Planned", "Emitting", "Committed"}
    => /\ optimizerProof
       /\ effectiveStream = SourceStream

EmittedIsExactSourcePrefix ==
  phase \in {"Emitting", "Committed", "Poisoned"}
    => emitted = SubSeq(SourceStream, 1, Len(emitted))

ReceiptMatchesEmission ==
  receipt = emitted

PublishedOnlyAfterCompleteReceipt ==
  phase # "Committed" => publishedCount = 0

NoRetryAfterEffect ==
  effectsStarted => retryCount = 0 /\ phase # "Working"

CommitRequiresCompleteReceipt ==
  phase = "Committed"
    => /\ emitted = SourceStream
       /\ receipt = SourceStream
       /\ publishedCount = SourceLength

Inv == TypeOK /\ ProjectionIsRepresentationPreserving
      /\ WorkingStateMatchesProjection
      /\ PersistentStateCommitsOnlyAfterReceipt
      /\ WorkingGenerationQualified
      /\ StateGenerationTraceExact
      /\ DedupAttributionExact
      /\ EmittedLogicalAttributionExact
      /\ OptimizerRequiresProof
      /\ EmittedIsExactSourcePrefix
      /\ ReceiptMatchesEmission
      /\ PublishedOnlyAfterCompleteReceipt
      /\ NoRetryAfterEffect
      /\ CommitRequiresCompleteReceipt

====
