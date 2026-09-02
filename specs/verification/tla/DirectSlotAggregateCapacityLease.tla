---- MODULE DirectSlotAggregateCapacityLease ----
(***************************************************************************)
(* R-BACK-2.104 aggregate retained-capacity lease. Two physical payloads   *)
(* are a symmetry reduction for queueCompatibility(32)'s 64 payloads; the *)
(* native production binding pins all 64 payloads and all 30 allocations.  *)
(* control shells and ReplayTransaction are absent because neither owns     *)
(* credit. Settlement tickets carry both generation and operation serial.  *)
(* The production projection is `LeaseHeld::tryAcquire` followed by         *)
(* `StagedDirectSlot::create(LeaseHeld&&)` and `commit() &&`: `Staged` is    *)
(* the only phase with staged credit, and commit moves directly to `Done`;  *)
(* LeaseHeld destruction is modeled by AllocationFailure. There is          *)
(* deliberately no typed edge from Done back to rollback.                   *)
(***************************************************************************)

EXTENDS Naturals, TLC

CONSTANTS AdoptionDiscipline, CreditDiscipline

Payloads == {1, 2}
AllocationDimensions == 30
AggregateLimit == 4
RequestedCapacity == 2

VARIABLES phase, owner, generations, retained, aggregateCredit, stagedCredit,
          adoptedDimensions, adoptionCounts, stagedGeneration, stagedSerial,
          nextSerial, settled, rejectedActions

vars == <<phase, owner, generations, retained, aggregateCredit, stagedCredit,
           adoptedDimensions, adoptionCounts, stagedGeneration, stagedSerial,
           nextSerial, settled, rejectedActions>>

Init ==
  /\ phase = "Idle"
  /\ owner = 0
  /\ generations = [p \in Payloads |-> 1]
  /\ retained = [p \in Payloads |-> 1]
  /\ aggregateCredit = 2
  /\ stagedCredit = 0
  /\ adoptedDimensions = 0
  /\ adoptionCounts = [p \in Payloads |-> 0]
  /\ stagedGeneration = 0
  /\ stagedSerial = 0
  /\ nextSerial = 1
  /\ settled = FALSE
  /\ rejectedActions = 0

Stage ==
  /\ phase = "Idle"
  /\ \E p \in Payloads:
       /\ owner' = p
       /\ phase' = "Staged"
       /\ stagedGeneration' = generations[p]
       /\ stagedSerial' = nextSerial
  /\ aggregateCredit + RequestedCapacity <= AggregateLimit
  /\ stagedCredit' = RequestedCapacity
  /\ nextSerial' = nextSerial + 1
  /\ settled' = FALSE
  /\ UNCHANGED <<generations, retained, aggregateCredit, adoptedDimensions,
                  adoptionCounts, rejectedActions>>

Adopt ==
  /\ phase = "Staged"
  /\ stagedGeneration = generations[owner]
  /\ stagedSerial > 0
  /\ phase' = "Done"
  /\ generations' = [generations EXCEPT ![owner] = @ + 1]
  /\ retained' = [retained EXCEPT ![owner] = RequestedCapacity]
  /\ aggregateCredit' = aggregateCredit - retained[owner] + RequestedCapacity
  /\ stagedCredit' = 0
  /\ stagedGeneration' = 0
  /\ stagedSerial' = 0
  /\ settled' = TRUE
  /\ adoptedDimensions' =
       IF AdoptionDiscipline = "Atomic" THEN AllocationDimensions ELSE 1
  /\ adoptionCounts' = [adoptionCounts EXCEPT ![owner] = @ + 1]
  /\ UNCHANGED <<owner, nextSerial, rejectedActions>>

AllocationFailure ==
  /\ phase = "Staged"
  /\ stagedGeneration = generations[owner]
  /\ stagedSerial > 0
  /\ phase' = "Done"
  /\ stagedCredit' = 0
  /\ aggregateCredit' =
       IF CreditDiscipline = "Conserved"
         THEN aggregateCredit
         ELSE aggregateCredit + stagedCredit
  /\ adoptedDimensions' = 0
  /\ stagedGeneration' = 0
  /\ stagedSerial' = 0
  /\ settled' = TRUE
  /\ UNCHANGED <<owner, generations, retained, adoptionCounts, nextSerial,
                  rejectedActions>>

(* These actions model callers presenting a generation/serial that is no
   longer current. They are explicit no-ops except for a bounded rejection
   count, so normal TLC traces exercise stale and double settlement without
   weakening the conservation invariants. *)
StaleAdopt ==
  /\ phase = "Staged"
  /\ rejectedActions < 2
  /\ rejectedActions' = rejectedActions + 1
  /\ UNCHANGED <<phase, owner, generations, retained, aggregateCredit,
                  stagedCredit, adoptedDimensions, adoptionCounts,
                  stagedGeneration, stagedSerial, nextSerial, settled>>

StaleRollback == StaleAdopt

DoubleSettlement ==
  /\ phase = "Done"
  /\ rejectedActions < 2
  /\ rejectedActions' = rejectedActions + 1
  /\ UNCHANGED <<phase, owner, generations, retained, aggregateCredit,
                  stagedCredit, adoptedDimensions, adoptionCounts,
                  stagedGeneration, stagedSerial, nextSerial, settled>>

Done ==
  /\ phase = "Done"
  /\ UNCHANGED vars

Next == Stage \/ Adopt \/ AllocationFailure \/ StaleAdopt \/ StaleRollback \/
        DoubleSettlement \/ Done

TypeOK ==
  /\ phase \in {"Idle", "Staged", "Done"}
  /\ owner \in Payloads \cup {0}
  /\ generations \in [Payloads -> 1..2]
  /\ retained \in [Payloads -> 1..RequestedCapacity]
  /\ aggregateCredit \in 0..AggregateLimit
  /\ stagedCredit \in 0..RequestedCapacity
  /\ adoptedDimensions \in 0..AllocationDimensions
  /\ adoptionCounts \in [Payloads -> 0..1]
  /\ stagedGeneration \in 0..2
  /\ stagedSerial \in 0..2
  /\ nextSerial \in 1..2
  /\ settled \in BOOLEAN
  /\ rejectedActions \in 0..2

TotalRetained == retained[1] + retained[2]

RetainedCreditConserved == aggregateCredit = TotalRetained
TransientCreditBounded == aggregateCredit + stagedCredit <= AggregateLimit
AdoptionIsAtomic == adoptedDimensions \in {0, AllocationDimensions}
GenerationConserved ==
  \A p \in Payloads: generations[p] = 1 + adoptionCounts[p]
NoCreditAfterFailedOrCompletedStage == phase /= "Done" \/ stagedCredit = 0
TicketIsGenerationQualified ==
  phase = "Staged" => stagedGeneration = generations[owner] /\ stagedSerial > 0
NoDoubleSettlement == phase = "Done" => settled

Spec == Init /\ [][Next]_vars

====
