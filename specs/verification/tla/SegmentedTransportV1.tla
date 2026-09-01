(*****************************************************************************)
(* Fixed-role SegmentedTransportV1 transport/refinement model.              *)
(*                                                                           *)
(* The model deliberately separates the three ordered roles:                *)
(*   ReserveRole -> AdoptRole -> EmitRole                                   *)
(*                                                                           *)
(* An immutable semantic batch is counted and deduplicated before any        *)
(* ownership or recorder effect.  ReserveAll owns the complete capacity     *)
(* claim; AdoptAll publishes the complete retained/ledger batch atomically;  *)
(* and ExactFixed emits one final contiguous CPU Tape extent.                 *)
(*                                                                           *)
(* Before adoption, rollback may restore the exact checkpoint and use one    *)
(* contiguous legacy fallback.  After adoption, or after an emission effect, *)
(* failure is effect-unknown and poisons the transport; it must not retry or  *)
(* fall back.  Waiting is a bounded condition-variable protocol: reclaim     *)
(* publishes capacity and a wake generation, and acquisition requires the     *)
(* observed generation to advance.                                            *)
(*                                                                           *)
(* This is a protocol model only.  It does not prove PE COM/bridge ABI,      *)
(* allocator internals, Objective-C/Metal ownership, or final pixels.        *)
*****************************************************************************)

---- MODULE SegmentedTransportV1 ----
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
  SegmentCount,
  CapacityDiscipline,
  AdoptionDiscipline,
  RollbackDiscipline,
  LedgerDiscipline,
  FallbackDiscipline,
  WakeDiscipline

Segments == 1 .. SegmentCount
Batch == <<1, 2, 3>>
Roles == {"ReserveRole", "AdoptRole", "EmitRole"}
Phases == {"Idle", "Waiting", "Reserved", "Adopted", "Emitted",
           "Settling", "Settled", "Fallback", "Poisoned"}

(* The reference batch is immutable and already pure: exact counts and the
   unique-handle count are values, not effects. *)
ExpectedRecordCount == SegmentCount
ExpectedHandleCount == 2
ExpectedPayloadBytes == SegmentCount
WireCapBytes == 32 * 1024 * 1024
(* These are the canonical D9C V2 values in include/dxmt9/device_c.h.  The
   fixed-role descriptor is a 144-byte POD value; its final 48 bytes are the
   capture token/event ordinal and immutable producer interval. Its role byte counts intentionally omit
   inter-table alignment, which the importer reconstructs from offsets. *)
WireHeaderBytes == 48
WireRecordBytes == 32
WireHandleBytes == 16
WireSectionDescriptorBytes == 16
SegmentedTransportDescriptorBytes == 144
CaptureTokenEventOrdinalBytes == 16
ProducerIdentityBytes == 32
WireAlignmentBytes == 8
AlignUp(value, alignment) == alignment * ((value + alignment - 1) \div alignment)
RecordTableOffset == WireHeaderBytes
HandleTableOffset == RecordTableOffset + ExpectedRecordCount * WireRecordBytes
PayloadTableEnd == HandleTableOffset + ExpectedHandleCount * WireHandleBytes
PayloadArenaOffset == AlignUp(PayloadTableEnd, WireAlignmentBytes)
InterTablePaddingBytes == PayloadArenaOffset - PayloadTableEnd
ExpectedWireBytes == PayloadArenaOffset + ExpectedPayloadBytes
ExpectedReservationUnits == ExpectedRecordCount + ExpectedHandleCount
                           + ExpectedPayloadBytes

BaseTape == <<0>>
BaseCapacity == 1
BaseRetains == 1
BaseLedger == 0

VARIABLES
  phase,
  role,
  checkpointTape,
  checkpointCapacity,
  checkpointRetains,
  checkpointLedger,
  tape,
  capacityUsed,
  retained,
  ledger,
  reservedRecords,
  reservedHandles,
  reservedPayload,
  adopted,
  emitted,
  settled,
  effects,
  fallbackCount,
  settlementOrder,
  capacityAvailable,
  wakeGeneration,
  observedWakeGeneration,
  waiting,
  waitUsed,
  rollbackOccurred,
  poisonReason

vars == <<phase, role, checkpointTape, checkpointCapacity, checkpointRetains,
  checkpointLedger, tape, capacityUsed, retained, ledger, reservedRecords,
  reservedHandles, reservedPayload, adopted, emitted, settled, effects,
  fallbackCount, settlementOrder, capacityAvailable, wakeGeneration,
  observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

Init ==
  /\ phase = "Idle"
  /\ role = "ReserveRole"
  /\ checkpointTape = BaseTape
  /\ checkpointCapacity = BaseCapacity
  /\ checkpointRetains = BaseRetains
  /\ checkpointLedger = BaseLedger
  /\ tape = BaseTape
  /\ capacityUsed = BaseCapacity
  /\ retained = BaseRetains
  /\ ledger = BaseLedger
  /\ reservedRecords = 0
  /\ reservedHandles = 0
  /\ reservedPayload = 0
  /\ adopted = 0
  /\ emitted = 0
  /\ settled = 0
  /\ effects = 0
  /\ fallbackCount = 0
  /\ settlementOrder = <<>>
  /\ capacityAvailable = TRUE
  /\ wakeGeneration = 0
  /\ observedWakeGeneration = 0
  /\ waiting = FALSE
  /\ waitUsed = FALSE
  /\ rollbackOccurred = FALSE
  /\ poisonReason = ""

(* A producer may wait for capacity; it does not poll or reserve a prefix. *)
RequestCapacityWait ==
  /\ phase = "Idle"
  /\ role = "ReserveRole"
  /\ capacityAvailable
  /\ waiting = FALSE
  /\ ~waitUsed
  /\ phase' = "Waiting"
  /\ capacityAvailable' = FALSE
  /\ waiting' = TRUE
  /\ waitUsed' = TRUE
  /\ observedWakeGeneration' = wakeGeneration
  /\ UNCHANGED <<role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape, capacityUsed, retained, ledger,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    settled, effects, fallbackCount, settlementOrder, wakeGeneration,
    rollbackOccurred, poisonReason>>

(* Reclaim is the only action that makes the denied capacity available.  The
   wake generation is the model's condition-variable notification witness. *)
ReclaimCapacity ==
  /\ phase = "Waiting"
  /\ waiting
  /\ ~capacityAvailable
  /\ capacityAvailable' = TRUE
  /\ wakeGeneration' = IF WakeDiscipline = "Signal"
                         THEN wakeGeneration + 1 ELSE wakeGeneration
  /\ UNCHANGED <<phase, role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape, capacityUsed, retained, ledger,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    settled, effects, fallbackCount, settlementOrder,
    observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

AcquireAfterWake ==
  /\ phase = "Waiting"
  /\ waiting
  /\ capacityAvailable
  /\ wakeGeneration > observedWakeGeneration
  /\ phase' = "Idle"
  /\ waiting' = FALSE
  /\ UNCHANGED <<role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape, capacityUsed, retained, ledger,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    settled, effects, fallbackCount, settlementOrder, capacityAvailable,
    wakeGeneration, observedWakeGeneration, waitUsed, rollbackOccurred, poisonReason>>

(* Pure count/dedup happens before this action and is represented by the
   exact fixed reservation tuple.  No retain, ledger, or Tape mutation is
   permitted before ReserveAll has reserved all three dimensions. *)
ReserveAll ==
  /\ phase = "Idle"
  /\ role = "ReserveRole"
  /\ capacityAvailable
  /\ reservedRecords = 0
  /\ reservedHandles = 0
  /\ reservedPayload = 0
  /\ capacityUsed' = checkpointCapacity +
       IF CapacityDiscipline = "All" THEN ExpectedReservationUnits
       ELSE ExpectedReservationUnits - 1
  /\ reservedRecords' = ExpectedRecordCount
  /\ reservedHandles' = ExpectedHandleCount
  /\ reservedPayload' = IF CapacityDiscipline = "All"
                         THEN ExpectedPayloadBytes
                         ELSE ExpectedPayloadBytes - 1
  /\ role' = "AdoptRole"
  /\ phase' = "Reserved"
  /\ UNCHANGED <<checkpointTape, checkpointCapacity, checkpointRetains,
    checkpointLedger, tape, retained, ledger, adopted, emitted, settled,
    effects, fallbackCount, settlementOrder, capacityAvailable,
    wakeGeneration, observedWakeGeneration, waiting, waitUsed, rollbackOccurred,
    poisonReason>>

(* Adoption is one atomic publication.  The intentionally unsafe mode
   publishes only one member, making the partial-adoption invariant fail. *)
AdoptAll ==
  /\ phase = "Reserved"
  /\ role = "AdoptRole"
  /\ reservedRecords = ExpectedRecordCount
  /\ reservedHandles = ExpectedHandleCount
  /\ reservedPayload = ExpectedPayloadBytes
  /\ adopted' = IF AdoptionDiscipline = "Atomic"
                 THEN ExpectedRecordCount ELSE 1
  /\ retained' = IF AdoptionDiscipline = "Atomic"
                 THEN checkpointRetains + ExpectedRecordCount
                 ELSE checkpointRetains + 1
  /\ ledger' = IF LedgerDiscipline = "Exact"
               THEN ExpectedRecordCount ELSE 1
  /\ role' = "EmitRole"
  /\ phase' = "Adopted"
  /\ UNCHANGED <<checkpointTape, checkpointCapacity, checkpointRetains,
    checkpointLedger, tape, capacityUsed, reservedRecords, reservedHandles,
    reservedPayload, emitted, settled, effects, fallbackCount,
    settlementOrder, capacityAvailable, wakeGeneration,
    observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

(* This is the post-adoption fail-stop edge.  It is reachable before any
   encoder effect and must not be converted into a retryable fallback. *)
PostAdoptionFailure ==
  /\ phase = "Adopted"
  /\ adopted > 0
  /\ phase' = "Poisoned"
  /\ poisonReason' = "post-adoption"
  /\ UNCHANGED <<role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape, capacityUsed, retained, ledger,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    settled, effects, fallbackCount, settlementOrder, capacityAvailable,
    wakeGeneration, observedWakeGeneration, waiting, waitUsed, rollbackOccurred>>

(* ExactFixed is a single final-layout emission: all records are contiguous,
   and it is the first visible transport effect. *)
EmitExactFixed ==
  /\ phase = "Adopted"
  /\ role = "EmitRole"
  /\ adopted = ExpectedRecordCount
  /\ ledger = ExpectedRecordCount
  /\ tape' = checkpointTape \o Batch
  /\ emitted' = ExpectedRecordCount
  /\ effects' = 1
  /\ role' = "EmitRole"
  /\ phase' = "Emitted"
  /\ UNCHANGED <<checkpointTape, checkpointCapacity, checkpointRetains,
    checkpointLedger, capacityUsed, retained, ledger, reservedRecords,
    reservedHandles, reservedPayload, adopted, settled, fallbackCount,
    settlementOrder, capacityAvailable, wakeGeneration,
    observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

(* Once the final emission has an effect, failure is poison and neither the
   legacy path nor ExactFixed may be retried. *)
PostEffectFailure ==
  /\ phase = "Emitted"
  /\ effects > 0
  /\ phase' = "Poisoned"
  /\ poisonReason' = "effect-unknown"
  /\ UNCHANGED <<role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape, capacityUsed, retained, ledger,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    settled, effects, fallbackCount, settlementOrder, capacityAvailable,
    wakeGeneration, observedWakeGeneration, waiting, waitUsed, rollbackOccurred>>

(* Pre-effect rollback is the only fallback edge.  A bad checkpoint discipline
   drops the old prefix/capacity, giving a short counterexample. *)
PreEffectFallback ==
  /\ phase = "Reserved"
  /\ role = "AdoptRole"
  /\ adopted = 0
  /\ effects = 0
  /\ phase' = "Fallback"
  /\ role' = "AdoptRole"
  /\ tape' = IF RollbackDiscipline = "RestoreCheckpoint"
             THEN checkpointTape \o Batch ELSE <<>>
  /\ capacityUsed' = IF RollbackDiscipline = "RestoreCheckpoint"
                      THEN checkpointCapacity ELSE 0
  /\ retained' = IF RollbackDiscipline = "RestoreCheckpoint"
                 THEN checkpointRetains ELSE 0
  /\ ledger' = IF RollbackDiscipline = "RestoreCheckpoint"
               THEN checkpointLedger ELSE 0
  /\ reservedRecords' = 0
  /\ reservedHandles' = 0
  /\ reservedPayload' = 0
  /\ fallbackCount' = 1
  /\ rollbackOccurred' = TRUE
  /\ UNCHANGED <<checkpointTape, checkpointCapacity, checkpointRetains,
    checkpointLedger, adopted, emitted, settled, effects, settlementOrder,
    capacityAvailable, wakeGeneration, observedWakeGeneration, waiting,
    waitUsed, poisonReason>>

(* Negative control only: allowing fallback after an effect duplicates the
   immutable batch and violates both fail-stop and at-most-once semantics. *)
FallbackAfterEffect ==
  /\ phase = "Emitted"
  /\ FallbackDiscipline = "AllowAfterEffect"
  /\ effects > 0
  /\ phase' = "Fallback"
  /\ role' = "AdoptRole"
  /\ tape' = tape \o Batch
  /\ fallbackCount' = fallbackCount + 1
  /\ UNCHANGED <<checkpointTape, checkpointCapacity, checkpointRetains,
    checkpointLedger, capacityUsed, retained, ledger, reservedRecords,
    reservedHandles, reservedPayload, adopted, emitted, settled, effects,
    settlementOrder, capacityAvailable, wakeGeneration,
    observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

SettleNext ==
  /\ phase = "Emitted"
  /\ role = "EmitRole"
  /\ emitted = ExpectedRecordCount
  /\ settled < ExpectedRecordCount
  /\ LET nextSegment == settled + 1 IN
       /\ settlementOrder' = Append(settlementOrder, nextSegment)
       /\ settled' = nextSegment
  /\ phase' = IF settled + 1 = ExpectedRecordCount
              THEN "Settled" ELSE "Emitted"
  /\ capacityUsed' = IF settled + 1 = ExpectedRecordCount
                      THEN checkpointCapacity ELSE capacityUsed
  /\ retained' = IF settled + 1 = ExpectedRecordCount
                 THEN checkpointRetains ELSE retained
  /\ ledger' = IF settled + 1 = ExpectedRecordCount
               THEN checkpointLedger ELSE ledger
  /\ UNCHANGED <<role, checkpointTape, checkpointCapacity,
    checkpointRetains, checkpointLedger, tape,
    reservedRecords, reservedHandles, reservedPayload, adopted, emitted,
    effects, fallbackCount, capacityAvailable, wakeGeneration,
    observedWakeGeneration, waiting, waitUsed, rollbackOccurred, poisonReason>>

Next == RequestCapacityWait \/ ReclaimCapacity \/ AcquireAfterWake
        \/ ReserveAll \/ AdoptAll \/ PostAdoptionFailure \/ EmitExactFixed
        \/ PostEffectFailure \/ PreEffectFallback \/ FallbackAfterEffect
        \/ SettleNext

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(ReclaimCapacity)
  /\ WF_vars(AcquireAfterWake)
  /\ WF_vars(ReserveAll)
  /\ WF_vars(AdoptAll)
  /\ WF_vars(EmitExactFixed)
  /\ WF_vars(SettleNext)

TypeOK ==
  /\ SegmentCount = 3
  /\ SegmentCount \in Nat
  /\ phase \in Phases
  /\ role \in Roles
  /\ checkpointTape \in Seq(Nat)
  /\ tape \in Seq(Nat)
  /\ checkpointCapacity \in Nat
  /\ checkpointRetains \in Nat
  /\ checkpointLedger \in Nat
  /\ capacityUsed \in Nat
  /\ retained \in Nat
  /\ ledger \in Nat
  /\ reservedRecords \in 0 .. ExpectedRecordCount
  /\ reservedHandles \in 0 .. ExpectedHandleCount
  /\ reservedPayload \in 0 .. ExpectedPayloadBytes
  /\ adopted \in 0 .. ExpectedRecordCount
  /\ emitted \in 0 .. ExpectedRecordCount
  /\ settled \in 0 .. ExpectedRecordCount
  /\ effects \in Nat
  /\ fallbackCount \in Nat
  /\ settlementOrder \in Seq(Nat)
  /\ capacityAvailable \in BOOLEAN
  /\ wakeGeneration \in Nat
  /\ observedWakeGeneration \in Nat
  /\ waiting \in BOOLEAN
  /\ waitUsed \in BOOLEAN
  /\ rollbackOccurred \in BOOLEAN
  /\ poisonReason \in {"", "post-adoption", "effect-unknown"}

PureCountsAndDedup ==
  /\ phase \in {"Reserved", "Adopted", "Emitted", "Settled"}
     => /\ reservedRecords = ExpectedRecordCount
        /\ reservedHandles = ExpectedHandleCount
        /\ reservedPayload = ExpectedPayloadBytes

WireExtentBounded ==
  /\ WireHeaderBytes = 48
  /\ WireRecordBytes = 32
  /\ WireHandleBytes = 16
  /\ WireSectionDescriptorBytes = 16
  /\ SegmentedTransportDescriptorBytes = 144
  /\ CaptureTokenEventOrdinalBytes = 16
  /\ ProducerIdentityBytes = 32
  /\ ExpectedWireBytes <= WireCapBytes

DescriptorShape ==
  /\ SegmentedTransportDescriptorBytes = WireHeaderBytes
       + 3 * (2 * 4 + 4 + 4) + CaptureTokenEventOrdinalBytes
       + ProducerIdentityBytes
  /\ SegmentedTransportDescriptorBytes = 144
  /\ InterTablePaddingBytes = PayloadArenaOffset - PayloadTableEnd

RoleOrder ==
  /\ phase \in {"Idle", "Waiting"} => role = "ReserveRole"
  /\ phase = "Reserved" => role = "AdoptRole"
  /\ phase = "Fallback" => role = "AdoptRole"
  /\ phase \in {"Adopted", "Emitted", "Settling", "Settled", "Poisoned"}
     => role = "EmitRole"

ReservationIsComplete ==
  /\ phase \in {"Reserved", "Adopted", "Emitted", "Settled"}
     => /\ reservedRecords = ExpectedRecordCount
        /\ reservedHandles = ExpectedHandleCount
        /\ reservedPayload = ExpectedPayloadBytes
        /\ (phase = "Settled" \/
            capacityUsed = checkpointCapacity + ExpectedReservationUnits)

AdoptionIsAtomic ==
  /\ phase \in {"Adopted", "Emitted", "Settled"}
     => adopted = ExpectedRecordCount

NoPartialPublication ==
  /\ phase \in {"Adopted", "Emitted"}
     => retained = checkpointRetains + ExpectedRecordCount
        /\ ledger = ExpectedRecordCount

CapacityConserved ==
  /\ phase = "Reserved"
     => capacityUsed = checkpointCapacity + ExpectedReservationUnits
  /\ phase = "Emitted"
     => capacityUsed = checkpointCapacity + ExpectedReservationUnits
  /\ phase \in {"Fallback", "Settled"}
     => capacityUsed = checkpointCapacity
  /\ phase = "Poisoned"
     => capacityUsed = checkpointCapacity + ExpectedReservationUnits

LedgerConserved ==
  /\ phase \in {"Adopted", "Emitted", "Settled"}
     => IF phase = "Settled"
        THEN ledger = checkpointLedger
        ELSE ledger = adopted
  /\ phase = "Fallback" => ledger = checkpointLedger

ExactFixedContiguous ==
  /\ phase \in {"Emitted", "Settled"}
     => tape = checkpointTape \o Batch

NoFallbackAfterEffect ==
  /\ fallbackCount > 0 => /\ effects = 0 /\ adopted = 0
                         /\ fallbackCount = 1

RollbackRestoresCheckpoint ==
  /\ rollbackOccurred
     => /\ phase = "Fallback"
        /\ capacityUsed = checkpointCapacity
        /\ retained = checkpointRetains
        /\ ledger = checkpointLedger

FIFOSettlement ==
  /\ settlementOrder = SubSeq(Batch, 1, Len(settlementOrder))
  /\ settled = Len(settlementOrder)

WakeOnReclaim ==
  /\ waiting /\ capacityAvailable
     => wakeGeneration > observedWakeGeneration

NoRetryAfterEffect ==
  /\ effects > 0 => fallbackCount = 0 /\ phase # "Fallback"

TerminalSettled ==
  phase = "Settled" => settlementOrder = Batch

Inv ==
  /\ TypeOK
  /\ PureCountsAndDedup
  /\ RoleOrder
  /\ ReservationIsComplete
  /\ DescriptorShape
  /\ AdoptionIsAtomic
  /\ NoPartialPublication
  /\ CapacityConserved
  /\ LedgerConserved
  /\ ExactFixedContiguous
  /\ NoFallbackAfterEffect
  /\ RollbackRestoresCheckpoint
  /\ FIFOSettlement
  /\ WakeOnReclaim
  /\ NoRetryAfterEffect
  /\ TerminalSettled

EventuallySettledOrFallbackOrPoisoned ==
  phase = "Idle" ~> phase \in {"Settled", "Fallback", "Poisoned"}

EventuallySettlementOrPoison ==
  phase = "Emitted" ~> phase \in {"Settled", "Poisoned"}

====
