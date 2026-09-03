---- MODULE DirectSourceLifecycle ----
(* ************************************************************************* *)
(* Bounded two-source/two-slot refinement of the production Direct source  *)
(* reducer. Source 1 reuses retained capacity in the first slot; source 2  *)
(* rotates to the second slot through staged/adopted capacity. Slot owners,  *)
(* storage/source generations, receipts, modes, and capacity phases are      *)
(* explicit state, rather than tautological helper functions.                *)
(*                                                                           *)
(* ReceiptPrepare moves credit into staged storage for the rotated source;   *)
(* Adopt is the atomic handoff back to retained storage. Thus conservation    *)
(* and staged ownership are exercised by the normal path. A Reclaimed edge   *)
(* never fabricates Encode or GPU completion. PoisonAbandoned is terminal    *)
(* and is not eligible for Reclaim. Destination selection is complete before *)
(* Admit/EffectCut; no post-effect transition changes the destination slot. *)
(* Production attachment is a separate FIFO row sequence: pre-effect         *)
(* cancellation removes only its tail and restores the attachment frontier.  *)
(* ************************************************************************* *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANT Fault

Sources == {1, 2}
Slots == {1, 2}
SemanticSpans == {<<1, 0>>, <<1, 1>>, <<2, 0>>}
Modes == {"Separator", "OrderedControl"}
ModeOrder == <<"Separator", "OrderedControl">>
Credit == 4
Terminal == {"Compatibility", "RolledBack", "Reclaimed", "FailStopped",
             "PoisonAbandoned"}

VARIABLES phase, imported, productionRows, admissionFrontier,
          published, reclaimed,
          witness, expectedWitness, witnessUses,
          effectSlot, effectSlotGeneration, effectStorageGeneration,
          emissions, effect, fallback, completed,
          restored, poisoned, retained, staged, detached, aggregate,
          semanticSpanCount, physicalCreditOwner, spanCredit,
          adopted, receiptPhase, capacityPhase,
          controlMode, modeIndex, hasPresent, presentPublished,
          slotOf, slotOwner, slotGeneration, storageGeneration,
          storageKind, storageAction, sourceGeneration,
          sourceStorageGeneration, slotCursor

vars == <<phase, imported, productionRows, admissionFrontier,
          published, reclaimed,
          witness, expectedWitness, witnessUses,
          effectSlot, effectSlotGeneration, effectStorageGeneration,
          emissions, effect, fallback, completed,
          restored, poisoned, retained, staged, detached, aggregate,
          semanticSpanCount, physicalCreditOwner, spanCredit,
          adopted, receiptPhase, capacityPhase,
          controlMode, modeIndex, hasPresent, presentPublished,
          slotOf, slotOwner, slotGeneration, storageGeneration,
          storageKind, storageAction, sourceGeneration,
          sourceStorageGeneration, slotCursor>>

Init ==
  /\ phase = [s \in Sources |-> "Unimported"]
  /\ imported = <<>>
  /\ productionRows = <<>>
  /\ admissionFrontier = 0
  /\ published = <<>>
  /\ reclaimed = <<>>
  /\ witness = [s \in Sources |->
       [slot |-> 0, slotGeneration |-> 0, storageGeneration |-> 0,
        sourceGeneration |-> 0, sourceStorageGeneration |-> 0]]
  /\ expectedWitness = [s \in Sources |->
       [slot |-> 0, slotGeneration |-> 0, storageGeneration |-> 0,
        sourceGeneration |-> 0, sourceStorageGeneration |-> 0]]
  /\ witnessUses = [s \in Sources |-> 0]
  /\ effectSlot = [s \in Sources |-> 0]
  /\ effectSlotGeneration = [s \in Sources |-> 0]
  /\ effectStorageGeneration = [s \in Sources |-> 0]
  /\ emissions = [s \in Sources |-> 0]
  /\ effect = [s \in Sources |-> FALSE]
  /\ fallback = [s \in Sources |-> FALSE]
  /\ completed = [s \in Sources |-> FALSE]
  /\ restored = [s \in Sources |-> FALSE]
  /\ poisoned = [s \in Sources |-> FALSE]
  /\ retained = [s \in Sources |-> 0]
  /\ staged = [s \in Sources |-> 0]
  /\ detached = [s \in Sources |-> 0]
  /\ aggregate = [s \in Sources |-> 0]
  /\ semanticSpanCount = [s \in Sources |-> IF s = 1 THEN 2 ELSE 1]
  /\ physicalCreditOwner = [s \in Sources |-> FALSE]
  /\ spanCredit = [x \in SemanticSpans |-> 0]
  /\ adopted = [s \in Sources |-> 0]
  /\ receiptPhase = [s \in Sources |-> "None"]
  /\ capacityPhase = [s \in Sources |-> "Unreserved"]
  /\ controlMode = [s \in Sources |-> "Unset"]
  /\ modeIndex = 1
  /\ hasPresent = [s \in Sources |-> FALSE]
  /\ presentPublished = FALSE
  /\ slotOf = [s \in Sources |-> 0]
  /\ slotOwner = [k \in Slots |-> 0]
  /\ slotGeneration = [k \in Slots |-> 0]
  /\ storageGeneration = [k \in Slots |-> 0]
  \* The admission-order counterexample starts with two legitimately retained
  \* slots so both sources can reach the production attachment gate before
  \* either witness is consumed. The nominal model still exercises rotation.
  /\ storageKind = [k \in Slots |->
       IF k = 1 \/ Fault = "AdmissionReorder" THEN "Retained" ELSE "Empty"]
  /\ storageAction = [s \in Sources |-> "Unset"]
  /\ sourceGeneration = [s \in Sources |-> 0]
  /\ sourceStorageGeneration = [s \in Sources |-> 0]
  /\ slotCursor = 1

NextSource(seq) == Len(seq) + 1
NextSlot(cursor) == IF cursor = 1 THEN 2 ELSE 1
\* CompatibilityReady has released the Direct destination and is waiting only
\* for its ordered compatibility publication; it is live but no longer a slot
\* owner.
Active(s) == phase[s] \notin {"Unimported", "CompatibilityReady"} /\
             phase[s] \notin Terminal
InSequence(value, seq) == \E i \in 1..Len(seq) : seq[i] = value
ZeroWitness ==
  [slot |-> 0, slotGeneration |-> 0, storageGeneration |-> 0,
   sourceGeneration |-> 0, sourceStorageGeneration |-> 0]

FreshWitness(s) ==
  [slot |-> slotOf[s],
   slotGeneration |-> slotGeneration[slotOf[s]],
   storageGeneration |-> storageGeneration[slotOf[s]],
   sourceGeneration |-> sourceGeneration[s],
   sourceStorageGeneration |-> sourceStorageGeneration[s]]

Import(s) ==
  /\ phase[s] = "Unimported"
  /\ (Fault = "SourceReorder" \/ s = NextSource(imported))
  /\ slotOwner[slotCursor] = 0
  /\ controlMode' = [controlMode EXCEPT ![s] = ModeOrder[modeIndex]]
  /\ modeIndex' = modeIndex + 1
  /\ phase' = [phase EXCEPT ![s] = "RawOwned"]
  /\ imported' = Append(imported, s)
  /\ slotOf' = [slotOf EXCEPT ![s] = slotCursor]
  /\ slotOwner' = [slotOwner EXCEPT ![slotCursor] = s]
  /\ slotGeneration' = [slotGeneration EXCEPT ![slotCursor] = @ + 1]
  /\ storageGeneration' =
       [storageGeneration EXCEPT ![slotCursor] = @ + 1]
  /\ sourceGeneration' =
       [sourceGeneration EXCEPT ![s] = slotGeneration[slotCursor] + 1]
  /\ sourceStorageGeneration' =
       [sourceStorageGeneration EXCEPT ![s] =
          storageGeneration[slotCursor] + 1]
  /\ storageAction' = [storageAction EXCEPT ![s] =
       IF storageKind[slotCursor] = "Retained"
       THEN "RetainedReuse" ELSE "StagedRotation"]
  /\ receiptPhase' = [receiptPhase EXCEPT ![s] = "None"]
  /\ retained' = [retained EXCEPT ![s] = Credit]
  /\ aggregate' = [aggregate EXCEPT ![s] = Credit]
  /\ physicalCreditOwner' = [physicalCreditOwner EXCEPT ![s] = TRUE]
  /\ spanCredit' = IF s = 1
       THEN [spanCredit EXCEPT ![<<1, 0>>] = Credit]
       ELSE [spanCredit EXCEPT ![<<2, 0>>] = Credit]
  /\ slotCursor' = NextSlot(slotCursor)
  /\ hasPresent' = [hasPresent EXCEPT ![s] = s = 2]
  /\ UNCHANGED <<published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, staged, detached, adopted, capacityPhase,
      presentPublished, storageKind, semanticSpanCount>>

Plan(s) ==
  /\ phase[s] = "RawOwned"
  /\ phase' = [phase EXCEPT ![s] = "Planned"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

Admit(s) ==
  /\ phase[s] = "Planned"
  \* Production attaches its observer row at this transition, so admission
  \* itself must advance the live reducer frontier. A preceding abstract raw
  \* may either own an earlier Direct row or already have fallen back through
  \* compatibility before this source attaches.
  /\ (Fault = "AdmissionReorder" \/
       (s > admissionFrontier /\
        (s = 1 \/ phase[1] = "Compatibility" \/
         InSequence(1, productionRows))))
  /\ adopted[s] = 2
  /\ capacityPhase[s] = "Adopted"
  /\ receiptPhase[s] = "LedgerQualified"
  /\ phase' = [phase EXCEPT ![s] = "Admitted"]
  /\ productionRows' = Append(productionRows, s)
  /\ admissionFrontier' = s
  /\ expectedWitness' = [expectedWitness EXCEPT ![s] = FreshWitness(s)]
  /\ witness' = [witness EXCEPT ![s] =
       IF Fault = "StaleWitness"
       THEN [slot |-> slotOf[s],
             slotGeneration |-> slotGeneration[slotOf[s]],
             storageGeneration |-> storageGeneration[slotOf[s]],
             sourceGeneration |-> sourceGeneration[s] + 100,
             sourceStorageGeneration |-> sourceStorageGeneration[s]]
       ELSE FreshWitness(s)]
  /\ UNCHANGED <<imported, published, reclaimed,
      witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

DuplicateWitness(s) ==
  /\ Fault = "DuplicateWitness"
  /\ phase[s] = "Admitted"
  /\ witnessUses' = [witnessUses EXCEPT ![s] = 2]
  /\ UNCHANGED <<phase, imported, published, reclaimed, witness,
      expectedWitness, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback,
      completed, restored, poisoned, retained, staged, detached, aggregate,
      adopted, receiptPhase, capacityPhase, controlMode, modeIndex,
      hasPresent, presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

EffectCut(s) ==
  /\ phase[s] = "Admitted"
  /\ witnessUses[s] = 0
  /\ witness[s] = expectedWitness[s]
  /\ witness[s] = FreshWitness(s)
  /\ phase' = [phase EXCEPT ![s] = "Effected"]
  /\ witnessUses' = [witnessUses EXCEPT ![s] = 1]
  /\ effect' = [effect EXCEPT ![s] = TRUE]
  /\ effectSlot' = [effectSlot EXCEPT ![s] = slotOf[s]]
  /\ effectSlotGeneration' = [effectSlotGeneration EXCEPT ![s] =
       slotGeneration[slotOf[s]]]
  /\ effectStorageGeneration' = [effectStorageGeneration EXCEPT ![s] =
       storageGeneration[slotOf[s]]]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      emissions, fallback, completed, restored, poisoned,
      retained, staged, detached, aggregate, adopted, receiptPhase,
      capacityPhase, controlMode, modeIndex, hasPresent, presentPublished,
      slotOf, slotOwner, slotGeneration, storageGeneration, storageKind,
      storageAction, sourceGeneration, sourceStorageGeneration, slotCursor>>

PostEffectDestinationChange(s) ==
  /\ Fault = "PostEffectDestinationChange"
  /\ phase[s] = "Effected"
  /\ LET oldSlot == slotOf[s]
         newSlot == NextSlot(oldSlot)
     IN /\ slotOwner[newSlot] = 0
        /\ slotOf' = [slotOf EXCEPT ![s] = newSlot]
        /\ slotOwner' = [slotOwner EXCEPT ![oldSlot] = 0, ![newSlot] = s]
        /\ slotGeneration' = [slotGeneration EXCEPT ![newSlot] = @ + 1]
        /\ storageGeneration' =
             [storageGeneration EXCEPT ![newSlot] = @ + 1]
        /\ sourceGeneration' = [sourceGeneration EXCEPT ![s] =
             slotGeneration[newSlot] + 1]
        /\ sourceStorageGeneration' = [sourceStorageGeneration EXCEPT ![s] =
             storageGeneration[newSlot] + 1]
  /\ UNCHANGED <<phase, imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, storageKind, storageAction, slotCursor,
      semanticSpanCount, physicalCreditOwner, spanCredit>>

PostEffectRetry(s) ==
  /\ Fault = "PostEffectRetry"
  /\ phase[s] = "Effected"
  /\ fallback' = [fallback EXCEPT ![s] = TRUE]
  /\ phase' = [phase EXCEPT ![s] = "RolledBack"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, completed, restored,
      poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

RollbackPreEffect(s) ==
  \* This is the production cancellation edge: admission created the row,
  \* no effect consumed its witness, and only the current FIFO tail may be
  \* erased. RawOwned/Planned fallback remains outside production observation.
  /\ Fault = "None"
  /\ phase[s] = "Admitted"
  /\ ~effect[s]
  /\ Len(productionRows) > 0
  /\ productionRows[Len(productionRows)] = s
  \* Erasing the Direct row does not itself execute the compatibility path.
  \* Keep that source pending until its semantic FIFO turn, otherwise a later
  \* source can either deadlock behind the missing ordinal or publish first.
  /\ phase' = [phase EXCEPT ![s] = "CompatibilityReady"]
  /\ productionRows' = IF Len(productionRows) = 1 THEN <<>>
       ELSE SubSeq(productionRows, 1, Len(productionRows) - 1)
  /\ admissionFrontier' = IF Len(productionRows) = 1 THEN 0
       ELSE productionRows[Len(productionRows) - 1]
  /\ witness' = [witness EXCEPT ![s] = ZeroWitness]
  /\ expectedWitness' = [expectedWitness EXCEPT ![s] = ZeroWitness]
  /\ fallback' = [fallback EXCEPT ![s] = TRUE]
  /\ slotOwner' = [slotOwner EXCEPT ![slotOf[s]] = 0]
  /\ UNCHANGED <<imported, published, reclaimed,
      witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect,
      completed, restored, poisoned, retained, staged, detached,
      aggregate, adopted, receiptPhase, capacityPhase, controlMode, modeIndex,
      hasPresent, presentPublished, slotOf, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

CompatibilityPublish(s) ==
  /\ phase[s] = "CompatibilityReady"
  /\ s = NextSource(published)
  /\ s = NextSource(reclaimed)
  /\ phase' = [phase EXCEPT ![s] = "Compatibility"]
  /\ published' = Append(published, s)
  \* Compatibility owns completion/reclaim outside the Direct reducer. Mark
  \* the source settled in the shared FIFO frontier without fabricating any
  \* Direct Completed/Restored phase or production row.
  /\ reclaimed' = Append(reclaimed, s)
  /\ emissions' = [emissions EXCEPT ![s] = @ + 1]
  /\ UNCHANGED <<imported, productionRows, admissionFrontier,
      witness, expectedWitness, witnessUses, effectSlot,
      effectSlotGeneration, effectStorageGeneration, effect, fallback,
      completed, restored, poisoned, retained, staged, detached, aggregate,
      semanticSpanCount, physicalCreditOwner, spanCredit, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

FailStop(s) ==
  /\ Fault = "FailStop"
  /\ phase[s] \in {"RawOwned", "Planned", "Admitted", "Effected",
      "ReceiptStaged", "ReceiptOwned", "Published", "Encoded",
      "Completed", "Detached", "Restored"}
  /\ phase' = [phase EXCEPT ![s] = "FailStopped"]
  /\ poisoned' = [poisoned EXCEPT ![s] = TRUE]
  /\ UNCHANGED <<imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect,
      fallback, completed, restored, retained, staged, detached, aggregate,
      adopted, receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

ReceiptPrepare(s) ==
  /\ phase[s] = "Planned"
  /\ adopted[s] = 0
  /\ phase' = [phase EXCEPT ![s] = "ReceiptStaged"]
  /\ receiptPhase' = [receiptPhase EXCEPT ![s] = "LedgerQualified"]
  /\ capacityPhase' = [capacityPhase EXCEPT ![s] =
       IF storageAction[s] = "StagedRotation" THEN "Staged" ELSE "Reserved"]
  /\ retained' = [retained EXCEPT ![s] =
       IF storageAction[s] = "StagedRotation" THEN 0 ELSE @]
  /\ staged' = [staged EXCEPT ![s] =
       IF storageAction[s] = "StagedRotation" THEN Credit ELSE @]
  /\ LET oldSlot == slotOf[s]
         newSlot == IF oldSlot = 1 THEN 2 ELSE 1
         rotates == storageAction[s] = "StagedRotation"
     IN /\ IF rotates THEN slotOwner[newSlot] = 0 ELSE TRUE
        /\ slotOf' = IF rotates
             THEN [slotOf EXCEPT ![s] = newSlot] ELSE slotOf
        /\ slotOwner' = IF rotates
             THEN [slotOwner EXCEPT ![oldSlot] = 0, ![newSlot] = s]
             ELSE slotOwner
        /\ slotGeneration' = IF rotates
             THEN [slotGeneration EXCEPT ![newSlot] = @ + 1]
             ELSE slotGeneration
        /\ storageGeneration' = IF rotates
             THEN [storageGeneration EXCEPT ![newSlot] = @ + 1]
             ELSE storageGeneration
        /\ sourceGeneration' = IF rotates
             THEN [sourceGeneration EXCEPT ![s] =
                 slotGeneration[newSlot] + 1] ELSE sourceGeneration
        /\ sourceStorageGeneration' = IF rotates
             THEN [sourceStorageGeneration EXCEPT ![s] =
                 storageGeneration[newSlot] + 1]
             ELSE sourceStorageGeneration
        /\ storageKind' = IF rotates
             THEN [storageKind EXCEPT ![newSlot] = "Staged"]
             ELSE [storageKind EXCEPT ![oldSlot] = "Retained"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed,
      restored, poisoned, detached, aggregate, adopted,
      controlMode, modeIndex, hasPresent, presentPublished,
      storageAction, slotCursor>>

Adopt(s) ==
  /\ phase[s] = "ReceiptStaged"
  /\ phase' = [phase EXCEPT ![s] = "Planned"]
  /\ capacityPhase' = [capacityPhase EXCEPT ![s] = "Adopted"]
  /\ adopted' = [adopted EXCEPT ![s] =
       IF Fault = "PartialAdoption" THEN 1 ELSE 2]
  /\ retained' = [retained EXCEPT ![s] = Credit]
  /\ staged' = [staged EXCEPT ![s] = 0]
  /\ storageKind' = [storageKind EXCEPT ![slotOf[s]] = "Adopted"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed,
      restored, poisoned, detached, aggregate, receiptPhase, controlMode,
      modeIndex, hasPresent, presentPublished, slotOf, slotOwner,
      slotGeneration, storageGeneration, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

DestinationReceipt(s) ==
  /\ phase[s] = "Effected"
  /\ receiptPhase[s] = "LedgerQualified"
  /\ phase' = [phase EXCEPT ![s] = "ReceiptOwned"]
  /\ UNCHANGED <<imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotOwner, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

Publish(s) ==
  /\ phase[s] = "ReceiptOwned"
  /\ (Fault = "SourceReorder" \/ s = NextSource(published))
  /\ phase' = [phase EXCEPT ![s] = "Published"]
  /\ published' = Append(published, s)
  /\ emissions' = [emissions EXCEPT ![s] = @ + 1]
  /\ presentPublished' = IF hasPresent[s] THEN TRUE ELSE presentPublished
  /\ UNCHANGED <<imported, reclaimed, witness, expectedWitness, witnessUses,
      effectSlot, effectSlotGeneration, effectStorageGeneration,
      effect, fallback, completed, restored, poisoned, retained, staged,
      detached, aggregate, adopted, receiptPhase, capacityPhase,
      controlMode, modeIndex, hasPresent, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

DoubleEmission(s) ==
  /\ Fault = "DoubleEmission"
  /\ phase[s] = "Published"
  /\ published' = Append(published, s)
  /\ emissions' = [emissions EXCEPT ![s] = @ + 1]
  /\ UNCHANGED <<phase, imported, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      effect, fallback, completed, restored, poisoned, retained,
      staged, detached, aggregate, adopted, receiptPhase, capacityPhase,
      controlMode, modeIndex, hasPresent, presentPublished, slotOf, slotOwner,
      slotGeneration, storageGeneration, storageKind, storageAction,
      sourceGeneration, sourceStorageGeneration, slotCursor>>

PhantomCredit(s) ==
  /\ Fault = "PhantomCredit"
  /\ phase[s] = "Completed"
  /\ detached' = [detached EXCEPT ![s] = Credit]
  /\ UNCHANGED <<phase, imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, aggregate, adopted, receiptPhase,
      capacityPhase, controlMode, modeIndex, hasPresent, presentPublished,
      slotOf, slotOwner, slotGeneration, storageGeneration, storageKind,
      storageAction, sourceGeneration, sourceStorageGeneration, slotCursor>>

LeakedCredit(s) ==
  /\ Fault = "LeakedCredit"
  /\ phase[s] = "Detached"
  /\ detached' = [detached EXCEPT ![s] = 0]
  /\ UNCHANGED <<phase, imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback, completed,
      restored, poisoned, retained, staged, aggregate, adopted, receiptPhase,
      capacityPhase, controlMode, modeIndex, hasPresent, presentPublished,
      slotOf, slotOwner, slotGeneration, storageGeneration, storageKind,
      storageAction, sourceGeneration, sourceStorageGeneration, slotCursor>>

Encode(s) ==
  /\ phase[s] = "Published"
  /\ phase' = [phase EXCEPT ![s] = "Encoded"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored, poisoned,
      retained, staged, detached, aggregate, adopted, receiptPhase,
      capacityPhase, controlMode, modeIndex, hasPresent, presentPublished,
      slotOf, slotOwner, slotGeneration, storageGeneration, storageKind,
      storageAction, sourceGeneration, sourceStorageGeneration, slotCursor>>

Complete(s) ==
  /\ phase[s] = "Encoded"
  /\ phase' = [phase EXCEPT ![s] = "Completed"]
  /\ completed' = [completed EXCEPT ![s] = TRUE]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, restored, poisoned, retained,
      staged, detached, aggregate, adopted, receiptPhase, capacityPhase,
      controlMode, modeIndex, hasPresent, presentPublished, slotOf, slotOwner,
      slotGeneration, storageGeneration, storageKind, storageAction,
      sourceGeneration, sourceStorageGeneration, slotCursor>>

DuplicateSharedCredit(s) ==
  /\ Fault = "DuplicateSharedCredit"
  /\ s = 1
  /\ phase[s] = "Completed"
  /\ spanCredit' = [spanCredit EXCEPT ![<<1, 1>>] = Credit]
  /\ UNCHANGED <<phase, imported, published, reclaimed, witness,
      expectedWitness, witnessUses, effectSlot, effectSlotGeneration,
      effectStorageGeneration, emissions, effect, fallback,
      completed, restored, poisoned, retained, staged, detached, aggregate,
      adopted, receiptPhase, capacityPhase, controlMode, modeIndex,
      hasPresent, presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageKind, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor, semanticSpanCount,
      physicalCreditOwner>>

EarlyReclaim(s) ==
  /\ Fault = "EarlyReclaim"
  /\ phase[s] = "Encoded"
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ reclaimed' = Append(reclaimed, s)
  /\ slotOwner' = [slotOwner EXCEPT ![slotOf[s]] = 0]
  /\ UNCHANGED <<imported, published, witness, expectedWitness, witnessUses,
      effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored,
      poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

Detach(s) ==
  /\ phase[s] = "Completed"
  /\ phase' = [phase EXCEPT ![s] = "Detached"]
  /\ capacityPhase' = [capacityPhase EXCEPT ![s] = "Detached"]
  /\ retained' = [retained EXCEPT ![s] = 0]
  /\ staged' = [staged EXCEPT ![s] = 0]
  /\ detached' = [detached EXCEPT ![s] = Credit]
  /\ storageKind' = [storageKind EXCEPT ![slotOf[s]] = "Detached"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored, poisoned,
      aggregate, adopted, receiptPhase, controlMode, modeIndex,
      hasPresent, presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

Restore(s) ==
  /\ phase[s] = "Detached"
  /\ phase' = [phase EXCEPT ![s] = "Restored"]
  /\ capacityPhase' = [capacityPhase EXCEPT ![s] = "Adopted"]
  /\ retained' = [retained EXCEPT ![s] = Credit]
  /\ detached' = [detached EXCEPT ![s] = 0]
  /\ restored' = [restored EXCEPT ![s] = TRUE]
  /\ storageKind' = [storageKind EXCEPT ![slotOf[s]] = "Retained"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, poisoned, staged,
      aggregate, adopted, receiptPhase, controlMode, modeIndex,
      hasPresent, presentPublished, slotOf, slotOwner, slotGeneration,
      storageGeneration, storageAction, sourceGeneration,
      sourceStorageGeneration, slotCursor>>

PoisonAbandon(s) ==
  /\ Fault \in {"PoisonAbandon", "PoisonReclaim"}
  /\ phase[s] = "Detached"
  /\ phase' = [phase EXCEPT ![s] = "PoisonAbandoned"]
  /\ retained' = [retained EXCEPT ![s] = Credit]
  /\ detached' = [detached EXCEPT ![s] = 0]
  /\ poisoned' = [poisoned EXCEPT ![s] = TRUE]
  /\ storageKind' = [storageKind EXCEPT ![slotOf[s]] = "Retained"]
  /\ UNCHANGED <<imported, published, reclaimed, witness, expectedWitness,
      witnessUses, effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed,
      restored, staged, aggregate, adopted, receiptPhase, capacityPhase,
      controlMode, modeIndex, hasPresent, presentPublished, slotOf,
      slotOwner, slotGeneration, storageGeneration, storageAction,
      sourceGeneration, sourceStorageGeneration, slotCursor>>

MissingRestoreReclaim(s) ==
  /\ Fault = "MissingRestore"
  /\ phase[s] = "Detached"
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ reclaimed' = Append(reclaimed, s)
  /\ slotOwner' = [slotOwner EXCEPT ![slotOf[s]] = 0]
  /\ UNCHANGED <<imported, published, witness, expectedWitness, witnessUses,
      effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored,
      poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

Reclaim(s) ==
  /\ phase[s] = "Restored"
  /\ (Fault = "SourceReorder" \/ s = NextSource(reclaimed))
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ reclaimed' = Append(reclaimed, s)
  /\ slotOwner' = [slotOwner EXCEPT ![slotOf[s]] = 0]
  /\ UNCHANGED <<imported, published, witness, expectedWitness, witnessUses,
      effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored, poisoned, retained,
      staged, detached, aggregate, adopted, receiptPhase, capacityPhase,
      controlMode, modeIndex, hasPresent, presentPublished, slotOf,
      slotGeneration, storageGeneration, storageKind, storageAction,
      sourceGeneration, sourceStorageGeneration, slotCursor>>

ReclaimPoisoned(s) ==
  /\ Fault = "PoisonReclaim"
  /\ phase[s] = "PoisonAbandoned"
  /\ phase' = [phase EXCEPT ![s] = "Reclaimed"]
  /\ reclaimed' = Append(reclaimed, s)
  /\ slotOwner' = [slotOwner EXCEPT ![slotOf[s]] = 0]
  /\ UNCHANGED <<imported, published, witness, expectedWitness, witnessUses,
      effectSlot, effectSlotGeneration, effectStorageGeneration,
      emissions, effect, fallback, completed, restored,
      poisoned, retained, staged, detached, aggregate, adopted,
      receiptPhase, capacityPhase, controlMode, modeIndex, hasPresent,
      presentPublished, slotOf, slotGeneration, storageGeneration,
      storageKind, storageAction, sourceGeneration, sourceStorageGeneration,
      slotCursor>>

PhysicalCreditMetadataStable ==
  UNCHANGED <<semanticSpanCount, physicalCreditOwner, spanCredit>>

ProductionProjectionStable ==
  UNCHANGED <<productionRows, admissionFrontier>>

StableStep(s) ==
  (Admit(s) \/ RollbackPreEffect(s) \/
   ((Plan(s) \/ DuplicateWitness(s) \/ EffectCut(s) \/
     PostEffectDestinationChange(s) \/ PostEffectRetry(s) \/ FailStop(s) \/
     CompatibilityPublish(s) \/
     ReceiptPrepare(s) \/ Adopt(s) \/ DestinationReceipt(s) \/
     Publish(s) \/ DoubleEmission(s) \/ PhantomCredit(s) \/ LeakedCredit(s) \/
     Encode(s) \/ Complete(s) \/ EarlyReclaim(s) \/ Detach(s) \/ Restore(s) \/
     PoisonAbandon(s) \/ MissingRestoreReclaim(s) \/ Reclaim(s) \/
     ReclaimPoisoned(s)) /\ ProductionProjectionStable))
  /\ PhysicalCreditMetadataStable

Step(s) ==
  ((Import(s) \/ DuplicateSharedCredit(s)) /\ ProductionProjectionStable) \/
  StableStep(s)


Done == \A s \in Sources : phase[s] \in Terminal
Next == (\E s \in Sources : Step(s)) \/ (Done /\ UNCHANGED vars)
Spec == Init /\ [][Next]_vars /\ \A s \in Sources : WF_vars(Step(s))

TypeOK ==
  /\ phase \in [Sources -> {"Unimported", "RawOwned", "Planned",
       "Admitted", "Effected", "ReceiptStaged", "ReceiptOwned",
       "Published", "Encoded", "Completed", "Detached", "Restored",
       "PoisonAbandoned", "CompatibilityReady", "Compatibility",
       "RolledBack", "Reclaimed",
       "FailStopped"}]
  /\ imported \in Seq(Sources)
  /\ productionRows \in Seq(Sources)
  /\ admissionFrontier \in 0..2
  /\ published \in Seq(Sources)
  /\ reclaimed \in Seq(Sources)
  /\ witness \in [Sources ->
       [slot: 0..2, slotGeneration: Nat, storageGeneration: Nat,
        sourceGeneration: Nat, sourceStorageGeneration: Nat]]
  /\ expectedWitness \in [Sources ->
       [slot: 0..2, slotGeneration: Nat, storageGeneration: Nat,
        sourceGeneration: Nat, sourceStorageGeneration: Nat]]
  /\ effectSlot \in [Sources -> 0..2]
  /\ effectSlotGeneration \in [Sources -> Nat]
  /\ effectStorageGeneration \in [Sources -> Nat]
  /\ semanticSpanCount \in [Sources -> 1..2]
  /\ physicalCreditOwner \in [Sources -> BOOLEAN]
  /\ spanCredit \in [SemanticSpans -> 0..Credit]
  /\ slotOf \in [Sources -> 0..2]
  /\ slotOwner \in [Slots -> 0..2]
  /\ controlMode \in [Sources -> (Modes \cup {"Unset"})]
  /\ receiptPhase \in [Sources -> {"None", "Exact", "LedgerQualified"}]
  /\ capacityPhase \in [Sources -> {"Unreserved", "Reserved", "Staged", "Adopted", "Detached"}]
  /\ storageKind \in [Slots -> {"Empty", "Retained", "Staged", "Adopted", "Detached"}]

ExactFifoSourceOrder ==
  /\ imported \in {<<>>, <<1>>, <<1, 2>>}
  /\ published \in {<<>>, <<1>>, <<1, 2>>}
  /\ reclaimed \in {<<>>, <<1>>, <<1, 2>>}

ProductionAdmissionFifo ==
  productionRows \in {<<>>, <<1>>, <<2>>, <<1, 2>>}

ProductionCancellationErasesRow ==
  /\ admissionFrontier =
       IF Len(productionRows) = 0 THEN 0
       ELSE productionRows[Len(productionRows)]
  /\ \A s \in Sources : phase[s] \in {"CompatibilityReady", "Compatibility"} =>
       /\ ~InSequence(s, productionRows)
       /\ witness[s] = ZeroWitness
       /\ expectedWitness[s] = ZeroWitness
       /\ slotOwner[slotOf[s]] # s

ExactlyOnce == \A s \in Sources : emissions[s] <= 1
WitnessIsFresh == \A s \in Sources : witness[s] = expectedWitness[s]
WitnessConsumedOnce == \A s \in Sources : witnessUses[s] <= 1
NoFallbackAfterEffect == \A s \in Sources : effect[s] => ~fallback[s]
CompletionBeforeReclaim ==
  \A s \in Sources : phase[s] = "Reclaimed" => completed[s]
CreditConservation ==
  \A s \in Sources : retained[s] + staged[s] + detached[s] = aggregate[s]

SemanticCredit(s) ==
  IF s = 1
  THEN spanCredit[<<1, 0>>] + spanCredit[<<1, 1>>]
  ELSE spanCredit[<<2, 0>>]

PhysicalCreditConservation ==
  \A s \in Sources :
    SemanticCredit(s) = aggregate[s] /\
    (physicalCreditOwner[s] <=> aggregate[s] = Credit)
RestoreBeforeReclaim ==
  \A s \in Sources : phase[s] = "Reclaimed" => restored[s] /\ ~poisoned[s]
AdoptionIsAtomic ==
  \A s \in Sources : adopted[s] = 0 \/
    (adopted[s] = 2 /\
      capacityPhase[s] \in {"Adopted", "Detached"} /\ staged[s] = 0)
PresentIsOrdered == presentPublished => published = <<1, 2>>

FinalDestinationReadyAtAdmission ==
  \A s \in Sources : phase[s] \in {"Admitted", "Effected", "ReceiptOwned",
      "Published", "Encoded", "Completed", "Detached", "Restored",
      "Reclaimed"} =>
    adopted[s] = 2 /\ capacityPhase[s] \in {"Adopted", "Detached"} /\
    slotOf[s] \in Slots

DestinationStableAfterEffect ==
  \A s \in Sources : effect[s] /\ phase[s] \in
      {"Effected", "ReceiptOwned", "Published", "Encoded", "Completed",
       "Detached", "Restored"} =>
    slotOf[s] = effectSlot[s] /\
    slotGeneration[slotOf[s]] = effectSlotGeneration[s] /\
    storageGeneration[slotOf[s]] = effectStorageGeneration[s]

ReceiptCapacityOwnership ==
  \A s \in Sources :
    (capacityPhase[s] = "Staged" => staged[s] = Credit) /\
    (capacityPhase[s] = "Adopted" => retained[s] = Credit /\ staged[s] = 0)

ReceiptBeforePublication ==
  \A s \in Sources :
    phase[s] \in {"ReceiptStaged", "ReceiptOwned", "Published", "Encoded",
      "Completed", "Detached", "Restored", "Reclaimed"} =>
        receiptPhase[s] = "LedgerQualified"

GenerationFreshness ==
  \A s \in Sources : Active(s) =>
    slotOf[s] \in Slots /\
    sourceGeneration[s] = slotGeneration[slotOf[s]] /\
    sourceStorageGeneration[s] = storageGeneration[slotOf[s]] /\
    sourceGeneration[s] > 0 /\ sourceStorageGeneration[s] > 0

NoDualSlotOwner ==
  /\ \A s \in Sources : Active(s) => slotOwner[slotOf[s]] = s
  /\ \A k \in Slots :
       Cardinality({s \in Sources : Active(s) /\ slotOf[s] = k}) <= 1

ControlModesCovered ==
  Done => {controlMode[s] : s \in Sources} = Modes

ReuseAndRotationOwned ==
  Done =>
    /\ {storageAction[s] : s \in Sources} = {"RetainedReuse", "StagedRotation"}
    /\ \E s \in Sources : capacityPhase[s] = "Adopted"
    /\ \E k \in Slots : slotGeneration[k] > 0 /\ storageGeneration[k] > 0

SharedSlotCreditOwnedOnce ==
  /\ semanticSpanCount[1] = 2
  /\ (physicalCreditOwner[1] =>
       Cardinality({x \in {<<1, 0>>, <<1, 1>>} : spanCredit[x] > 0}) = 1)
  /\ (physicalCreditOwner[1] =>
       spanCredit[<<1, 0>>] + spanCredit[<<1, 1>>] = aggregate[1])
  /\ (~physicalCreditOwner[1] =>
       spanCredit[<<1, 0>>] + spanCredit[<<1, 1>>] = 0)

LifecycleProgress ==
  \A s \in Sources : phase[s] = "Admitted" ~> phase[s] \in Terminal

=============================================================================
