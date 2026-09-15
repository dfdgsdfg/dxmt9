---- MODULE DirectSlotDeniedOwnership ----
(* R-BACK-2.104/2.105: optional denial must not erase existing capacity
   authority. The queue mutex serializes these physical transitions; source
   execution/completion/reclaim are abstract interfaces, not GPU pixels.
   Exact fallback is mandatory source storage, not optional headroom. *)
EXTENDS Naturals, TLC
CONSTANTS ReceiptDiscipline, GrowthDiscipline, ReclaimDiscipline
VARIABLES phase, ownerValid, covered, generation, receipt, physical,
          retained, staged, effects, completions, ordinaryDone, writeWitness,
          writeFault
vars == <<phase, ownerValid, covered, generation, receipt, physical,
          retained, staged, effects, completions, ordinaryDone, writeWitness,
          writeFault>>

(* Same four rows as reduceDirectSlotExactFallback's native truth table. *)
FallbackAction(valid, fits) ==
  IF ~valid THEN "Reject" ELSE IF fits THEN "Reuse" ELSE "Replace"

Init ==
  /\ phase = "Denied"
  /\ ownerValid \in BOOLEAN /\ covered \in BOOLEAN
  /\ generation = 1 /\ receipt = 1
  /\ physical = 1 /\ retained = 1 /\ staged = 0
  /\ effects = 0 /\ completions = 0
  /\ ordinaryDone = FALSE /\ writeWitness = 0 /\ writeFault = FALSE

Recover ==
  /\ phase = "Denied"
  /\ LET action == FallbackAction(ownerValid, covered) IN
       /\ phase' = CASE action = "Reject" -> "Failed"
                       [] action = "Reuse" -> "Admitted"
                       [] OTHER -> "Staged"
       /\ staged' = IF action = "Replace" THEN 2 ELSE 0
  /\ receipt' = IF ReceiptDiscipline = "Downgrade" THEN 0 ELSE receipt
  /\ UNCHANGED <<ownerValid, covered, generation, physical, retained,
                  effects, completions, ordinaryDone, writeWitness, writeFault>>

Adopt ==
  /\ phase = "Staged"
  /\ phase' = "Admitted"
  /\ physical' = 2 /\ generation' = 2 /\ receipt' = 2
  /\ retained' = IF GrowthDiscipline = "Unpriced" THEN retained ELSE 2
  /\ staged' = 0
  /\ UNCHANGED <<ownerValid, covered, effects, completions,
                  ordinaryDone, writeWitness, writeFault>>

AllocationFailure ==
  /\ phase = "Staged" /\ phase' = "Failed" /\ staged' = 0
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, physical,
                  retained, effects, completions, ordinaryDone, writeWitness,
                  writeFault>>

(* One mutex-held ordinary append/batch after Direct reuse/replacement.
   No publication, completion or other admission can interleave in this scope.
   The allocation-fault arm retains and settles successful earlier reserves. *)
BeginOrdinary ==
  /\ phase = "Admitted" /\ ~ordinaryDone
  /\ phase' = "OrdinaryOpen"
  /\ writeWitness' = IF ReceiptDiscipline = "LateWitness" THEN 0 ELSE receipt
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, physical, retained,
                  staged, effects, completions, ordinaryDone, writeFault>>

GrowOrdinary ==
  /\ phase = "OrdinaryOpen" /\ phase' = "OrdinaryGrown"
  /\ physical' = physical + 1 /\ writeFault' \in BOOLEAN
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, retained,
                  staged, effects, completions, ordinaryDone, writeWitness>>

SettleOrdinary ==
  /\ phase \in {"OrdinaryOpen", "OrdinaryGrown"}
  /\ writeWitness = receipt /\ writeWitness # 0
  /\ phase' = IF writeFault THEN "Stopped" ELSE "Admitted"
  /\ LET changed == physical # retained IN
       /\ generation' = IF changed THEN generation + 1 ELSE generation
       /\ receipt' = generation'
  /\ retained' = IF GrowthDiscipline = "UnaccountedOrdinary"
                 THEN retained ELSE physical
  /\ ordinaryDone' = TRUE /\ writeWitness' = 0
  /\ UNCHANGED <<ownerValid, covered, physical, staged, effects, completions,
                  writeFault>>

Execute ==
  /\ phase = "Admitted" /\ phase' = "Published" /\ effects' = effects + 1
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, physical,
                  retained, staged, completions, ordinaryDone, writeWitness,
                  writeFault>>

Complete ==
  /\ phase = "Published" /\ phase' = "Completed" /\ completions' = 1
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, physical,
                  retained, staged, effects, ordinaryDone, writeWitness, writeFault>>

Reclaim ==
  /\ phase = "Completed" \/
      (ReclaimDiscipline = "Early" /\ phase = "Published")
  /\ phase' = "Reclaimed"
  /\ UNCHANGED <<ownerValid, covered, generation, receipt, physical,
                  retained, staged, effects, completions, ordinaryDone,
                  writeWitness, writeFault>>

Next == Recover \/ Adopt \/ AllocationFailure \/ BeginOrdinary \/
        GrowOrdinary \/ SettleOrdinary \/ Execute \/ Complete \/ Reclaim
TypeOK ==
  /\ phase \in {"Denied", "Staged", "Admitted", "Failed", "Published", "Completed", "Reclaimed",
                "OrdinaryOpen", "OrdinaryGrown", "Stopped"}
  /\ ownerValid \in BOOLEAN /\ covered \in BOOLEAN
  /\ generation \in 1..3 /\ receipt \in 0..3
  /\ physical \in 1..3 /\ retained \in 1..3 /\ staged \in {0, 2}
  /\ ordinaryDone \in BOOLEAN /\ writeWitness \in 0..3 /\ writeFault \in BOOLEAN
  /\ effects \in 0..1 /\ completions \in 0..1
AuthorityPreserved == receipt = generation
PhysicalCreditConserved == retained = physical \/
  (phase = "OrdinaryGrown" /\ physical = retained + 1 /\ writeWitness = receipt)
ScopedWriteHasAuthority ==
  phase \in {"OrdinaryOpen", "OrdinaryGrown"} => writeWitness = receipt /\ receipt # 0
StoppedBeforeEffects == phase = "Stopped" => effects = 0 /\ retained = physical
FailureAtomic == phase = "Failed" =>
  /\ generation = 1 /\ physical = 1 /\ retained = 1 /\ staged = 0 /\ effects = 0
ValidOwnerBeforeEffects == effects > 0 => ownerValid
ExactlyOnce == effects <= 1 /\ completions <= effects
CompletionBeforeReclaim == phase = "Reclaimed" => completions = 1
NoLeakedStage == (phase /= "Staged") => staged = 0
Progress == <>(phase \in {"Failed", "Stopped", "Reclaimed"})
Spec == Init /\ [][Next]_vars /\ WF_vars(Next)
====
