---- MODULE LockScopeRefinement ----
(******************************************************************************
 * Bounded lock-scope refinement for the experimental R-BACK-SYNC lane.
 *
 * This is deliberately a small model of three independent obligations:
 *
 *   1. A range upload is admitted and applied while it owns the buffer-arena
 *      exclusive scope.  It carries the caller's logical identity/generation,
 *      requires a disjoint NOOVERWRITE range, and changes only those bytes.
 *      No queue sequence or completion watermark is part of this path.
 *
 *   2. A bounded capture batch records the exact logical generation and
 *      concrete backing.  Captured marks are set-like (A, B, A is one set of
 *      two concrete marks), while logical/current-backing marks are flush
 *      fences qualified by the rotation epoch.  The batch sequence is fixed
 *      until flush, and the scalar waterline reference is compared with the
 *      batched result before publication.
 *
 *   3. Off-queue preparation may retain its input and compute only immutable
 *      payload/entry/pair facts.  Live destination validation, binding
 *      capture, slot capacity/append, sequence assignment, and publication
 *      are queue-held.  ValidateDestination is called after acquisition and
 *      before any effect.
 *
 * The four alternate implementations are intentionally narrow regression
 * controls.  They are executed by scripts/check/verify_tla.sh as expected
 * failures and are not claims about driver bytes, Metal, or final pixels.
 *
 * Scope exclusions are deliberate: ActiveGeneration is one bounded generation
 * only, so generation reuse/reclaim and stale-generation pinning are supplied
 * by the native Pool fixture (and its existing resource-lifetime refinements),
 * not proved here.  The only liveness claim is bounded flush progress under
 * weak fairness; queue drain, reclaim, shutdown, and general scheduling
 * liveness are outside this model.
 ******************************************************************************)

EXTENDS Naturals, FiniteSets

CONSTANTS
  Positions,
  ReadPositions,
  WritePositions,
  InitialReadValue,
  ApplicationValue,
  LogicalHandle,
  ActiveGeneration,
  Backings,
  CollapsedBacking,
  MaxCaptures,
  Implementation

ASSUME Positions # {}
ASSUME ReadPositions \subseteq Positions
ASSUME WritePositions \subseteq Positions
ASSUME ReadPositions \cap WritePositions = {}
ASSUME InitialReadValue \in Nat
ASSUME ApplicationValue \in Nat
ASSUME Backings # {}
ASSUME CollapsedBacking \in Backings
ASSUME MaxCaptures \in Nat /\ MaxCaptures >= 1
ASSUME Implementation \in {"Production", "WrongBackingCollapse",
                             "MissingPrepublishFlush", "UnretainedInput",
                             "StaleDestination"}

CaptureSlots == 1..MaxCaptures

RangePhases == {"Idle", "Admitted", "Uploaded", "Done"}
BatchPhases == {"Idle", "Open", "Flushed", "Published"}
PlannerPhases == {"Idle", "Planned", "Rejected", "Applied"}
QueueOwners == {"Free", "Held"}
ArenaOwners == {"Free", "Range"}

VARIABLES
  (* Range-upload arena scope and abstract bytes. *)
  arenaOwner,
  rangePhase,
  rangeIdentity,
  rangeGeneration,
  rangeAdmitted,
  rangeUploaded,
  rangeShadow,
  rangeMetal,

  (* Captured-backing batch and its scalar reference. *)
  batchPhase,
  batchSeq,
  finalSeq,
  currentBacking,
  currentGeneration,
  rotationEpoch,
  captureCount,
  capturedStamps,
  expectedStamps,
  capturedMarkKeys,
  flushFenceKeys,
  logicalWaterline,
  backingWaterlines,
  referenceLogicalWaterline,
  referenceBackingWaterlines,
  publishedLogicalWaterline,
  publishedBackingWaterlines,

  (* Immutable off-queue preparation and queue-held finalization. *)
  plannerPhase,
  inputRetained,
  plannedPayloadSize,
  plannedEntryValid,
  plannedPairCompatible,
  plannedDestinationCaptured,
  plannedDestinationValid,
  queueOwner,
  queueStopped,
  arenaActive,
  directActive,
  destinationValidated,
  effectApplied,
  sequenceAssigned,
  published,
  publicationWhileQueueHeld

vars == <<
  arenaOwner, rangePhase, rangeIdentity, rangeGeneration, rangeAdmitted,
  rangeUploaded, rangeShadow, rangeMetal,
  batchPhase, batchSeq, finalSeq, currentBacking, currentGeneration,
  rotationEpoch, captureCount, capturedStamps, expectedStamps,
  capturedMarkKeys, flushFenceKeys, logicalWaterline, backingWaterlines,
  referenceLogicalWaterline, referenceBackingWaterlines,
  publishedLogicalWaterline, publishedBackingWaterlines,
  plannerPhase, inputRetained, plannedPayloadSize, plannedEntryValid,
  plannedPairCompatible, plannedDestinationCaptured, plannedDestinationValid,
  queueOwner, queueStopped, arenaActive, directActive, destinationValidated,
  effectApplied, sequenceAssigned, published, publicationWhileQueueHeld>>

RangeVars == <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
               rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal>>

BatchVars == <<batchPhase, batchSeq, finalSeq, currentBacking,
               currentGeneration, rotationEpoch, captureCount,
               capturedStamps, expectedStamps, capturedMarkKeys,
               flushFenceKeys, logicalWaterline, backingWaterlines,
               referenceLogicalWaterline, referenceBackingWaterlines,
               publishedLogicalWaterline, publishedBackingWaterlines>>

PlannerVars == <<plannerPhase, inputRetained, plannedPayloadSize,
                  plannedEntryValid, plannedPairCompatible,
                  plannedDestinationCaptured, plannedDestinationValid,
                  queueOwner, queueStopped, arenaActive, directActive,
                  destinationValidated, effectApplied, sequenceAssigned,
                  published, publicationWhileQueueHeld>>

(* ------------------------------------------------------------------------- *)
(* Pure vocabulary shared by the native fixture.                              *)

Max(a, b) == IF a >= b THEN a ELSE b

(* C++: exact logical handle/generation/concrete-backing capture. *)
Stamp(logical, generation, backing) ==
  [logical |-> logical, generation |-> generation, backing |-> backing]

(* C++: captured mark deduplication key.  It contains concrete identity. *)
CapturedMarkKey(stamp) ==
  [kind |-> "captured", logical |-> stamp.logical,
   generation |-> stamp.generation, backing |-> stamp.backing]

(* C++: logical/current-backing marks are fences, not captured-set entries.
 * Rotation epoch prevents a fence on the new active backing from collapsing
 * into a fence emitted before the rotation. *)
LogicalFenceKey ==
  [kind |-> "logical", logical |-> LogicalHandle,
   generation |-> currentGeneration, epoch |-> rotationEpoch]

BackingFenceKey ==
  [kind |-> "current-backing", backing |-> currentBacking,
   generation |-> currentGeneration, epoch |-> rotationEpoch]

CurrentFenceMarked ==
  /\ LogicalFenceKey \in flushFenceKeys
  /\ BackingFenceKey \in flushFenceKeys

AllCapturedMarked ==
  \A stamp \in capturedStamps : CapturedMarkKey(stamp) \in capturedMarkKeys

(* Scalar serial reference: it is derived from the expected capture stream,
 * independently of the set used by the batched implementation. *)
ExpectedLogicalWaterline ==
  IF expectedStamps # {}
     \/ (\E key \in flushFenceKeys : key.kind = "logical")
  THEN batchSeq ELSE 0

ExpectedBackingWaterlines ==
  [b \in Backings |->
    IF (\E stamp \in expectedStamps : stamp.backing = b)
       \/ (\E key \in flushFenceKeys :
             /\ key.kind = "current-backing"
             /\ key.backing = b)
    THEN batchSeq ELSE 0]

(* C++: preparation's post-acquire destination predicate. *)
ValidateDestination(stopped, arenaIsActive, directIsActive) ==
  /\ ~stopped
  /\ ~arenaIsActive
  /\ ~directIsActive

(* ------------------------------------------------------------------------- *)
(* Initialization.                                                            *)

Init ==
  /\ arenaOwner = "Free"
  /\ rangePhase = "Idle"
  /\ rangeIdentity = LogicalHandle
  /\ rangeGeneration = ActiveGeneration
  /\ rangeAdmitted = FALSE
  /\ rangeUploaded = FALSE
  /\ rangeShadow = [p \in Positions |->
                    IF p \in ReadPositions THEN InitialReadValue ELSE 0]
  /\ rangeMetal = [p \in Positions |->
                   IF p \in ReadPositions THEN InitialReadValue ELSE 0]

  /\ batchPhase = "Idle"
  /\ batchSeq = 0
  /\ finalSeq = 0
  /\ currentBacking = CollapsedBacking
  /\ currentGeneration = ActiveGeneration
  /\ rotationEpoch = 0
  /\ captureCount = 0
  /\ capturedStamps = {}
  /\ expectedStamps = {}
  /\ capturedMarkKeys = {}
  /\ flushFenceKeys = {}
  /\ logicalWaterline = 0
  /\ backingWaterlines = [b \in Backings |-> 0]
  /\ referenceLogicalWaterline = 0
  /\ referenceBackingWaterlines = [b \in Backings |-> 0]
  /\ publishedLogicalWaterline = 0
  /\ publishedBackingWaterlines = [b \in Backings |-> 0]

  /\ plannerPhase = "Idle"
  /\ inputRetained = FALSE
  /\ plannedPayloadSize = 0
  /\ plannedEntryValid = FALSE
  /\ plannedPairCompatible = FALSE
  /\ plannedDestinationCaptured = FALSE
  /\ plannedDestinationValid = FALSE
  /\ queueOwner = "Free"
  /\ queueStopped = FALSE
  /\ arenaActive = FALSE
  /\ directActive = FALSE
  /\ destinationValidated = FALSE
  /\ effectApplied = FALSE
  /\ sequenceAssigned = FALSE
  /\ published = FALSE
  /\ publicationWhileQueueHeld = FALSE

(* ------------------------------------------------------------------------- *)
(* Range upload: the only lock protecting this state is the arena lock.        *)

(* Immutable caller-owned source bytes.  The mutable pool shadow is changed
 * only after the arena lock is held by StageRangeBytes. *)
InputBytes ==
  [p \in Positions |->
    IF p \in ReadPositions THEN InitialReadValue ELSE
      IF p \in WritePositions THEN ApplicationValue ELSE 0]

RangeWriteShadow == InputBytes

AcquireRange ==
  /\ rangePhase = "Idle"
  /\ arenaOwner = "Free"
  /\ arenaOwner' = "Range"
  /\ rangePhase' = "Idle"
  /\ UNCHANGED <<rangeIdentity, rangeGeneration, rangeAdmitted,
                 rangeUploaded, rangeShadow, rangeMetal>>
  /\ UNCHANGED <<BatchVars, PlannerVars>>

StageRangeBytes ==
  /\ rangePhase = "Idle"
  /\ arenaOwner = "Range"
  /\ rangeShadow' = RangeWriteShadow
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeMetal>>
  /\ UNCHANGED <<BatchVars, PlannerVars>>

AdmitRange ==
  /\ arenaOwner = "Range"
  /\ rangePhase = "Idle"
  /\ rangeIdentity = LogicalHandle
  /\ rangeGeneration = ActiveGeneration
  /\ ReadPositions \cap WritePositions = {}
  /\ rangeAdmitted' = TRUE
  /\ rangePhase' = "Admitted"
  /\ UNCHANGED <<arenaOwner, rangeIdentity, rangeGeneration, rangeUploaded,
                 rangeShadow, rangeMetal>>
  /\ UNCHANGED <<BatchVars, PlannerVars>>

ApplyRange ==
  /\ arenaOwner = "Range"
  /\ rangePhase = "Admitted"
  /\ rangeAdmitted = TRUE
  /\ rangeShadow = InputBytes
  /\ rangeMetal' = [p \in Positions |->
                     IF p \in WritePositions THEN rangeShadow[p]
                     ELSE rangeMetal[p]]
  /\ rangeUploaded' = TRUE
  /\ rangePhase' = "Uploaded"
  /\ UNCHANGED <<arenaOwner, rangeIdentity, rangeGeneration, rangeAdmitted,
                 rangeShadow>>
  /\ UNCHANGED <<BatchVars, PlannerVars>>

ReleaseRange ==
  /\ arenaOwner = "Range"
  /\ rangePhase = "Uploaded"
  /\ arenaOwner' = "Free"
  /\ rangePhase' = "Done"
  /\ UNCHANGED <<rangeIdentity, rangeGeneration, rangeAdmitted,
                 rangeUploaded, rangeShadow, rangeMetal>>
  /\ UNCHANGED <<BatchVars, PlannerVars>>

(* ------------------------------------------------------------------------- *)
(* Batch capture / mark / flush.                                               *)

BeginBatch ==
  /\ batchPhase = "Idle"
  /\ batchPhase' = "Open"
  /\ batchSeq' = 1
  /\ UNCHANGED <<finalSeq, currentBacking, currentGeneration, rotationEpoch,
                 captureCount, capturedStamps, expectedStamps,
                 capturedMarkKeys, flushFenceKeys, logicalWaterline,
                 backingWaterlines, referenceLogicalWaterline,
                 referenceBackingWaterlines, publishedLogicalWaterline,
                 publishedBackingWaterlines>>
  /\ UNCHANGED <<RangeVars, PlannerVars>>

Capture ==
  /\ batchPhase = "Open"
  /\ captureCount < MaxCaptures
  /\ LET expected == Stamp(LogicalHandle, currentGeneration, currentBacking)
         stored == Stamp(LogicalHandle, currentGeneration,
                         IF Implementation = "WrongBackingCollapse"
                         THEN CollapsedBacking ELSE currentBacking)
     IN
       /\ capturedStamps' = capturedStamps \cup {stored}
       /\ expectedStamps' = expectedStamps \cup {expected}
       /\ captureCount' = captureCount + 1
  /\ UNCHANGED <<batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines>>
  /\ UNCHANGED <<RangeVars, PlannerVars>>

MarkCaptured ==
  /\ batchPhase = "Open"
  /\ capturedStamps # {}
  /\ \E stamp \in capturedStamps :
       LET key == CapturedMarkKey(stamp) IN
         /\ capturedMarkKeys' = capturedMarkKeys \cup {key}
         /\ logicalWaterline' = Max(logicalWaterline, batchSeq)
         /\ backingWaterlines' =
              [backingWaterlines EXCEPT ![stamp.backing] =
                 Max(@, batchSeq)]
         /\ UNCHANGED <<referenceLogicalWaterline,
                        referenceBackingWaterlines>>
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, flushFenceKeys,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueOwner, queueStopped, arenaActive, directActive,
                 destinationValidated, effectApplied, sequenceAssigned,
                 published, publicationWhileQueueHeld>>

MarkCurrentFence ==
  /\ batchPhase = "Open"
  /\ AllCapturedMarked
  /\ flushFenceKeys' = flushFenceKeys \cup {LogicalFenceKey, BackingFenceKey}
  /\ logicalWaterline' = Max(logicalWaterline, batchSeq)
  /\ backingWaterlines' =
       [backingWaterlines EXCEPT ![currentBacking] = Max(@, batchSeq)]
  /\ UNCHANGED <<referenceLogicalWaterline,
                 referenceBackingWaterlines>>
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueOwner, queueStopped, arenaActive, directActive,
                 destinationValidated, effectApplied, sequenceAssigned,
                 published, publicationWhileQueueHeld>>

RotateBacking ==
  /\ batchPhase = "Open"
  /\ captureCount < MaxCaptures
  /\ rotationEpoch < 2
  /\ CurrentFenceMarked
  /\ currentBacking' \in Backings
  /\ currentBacking' # currentBacking
  /\ rotationEpoch' = rotationEpoch + 1
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentGeneration,
                 captureCount, capturedStamps, expectedStamps,
                 capturedMarkKeys, flushFenceKeys, logicalWaterline,
                 backingWaterlines, referenceLogicalWaterline,
                 referenceBackingWaterlines, publishedLogicalWaterline,
                 publishedBackingWaterlines, plannerPhase, inputRetained,
                 plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

FlushBatch ==
  /\ batchPhase = "Open"
  /\ captureCount = MaxCaptures
  /\ AllCapturedMarked
  /\ CurrentFenceMarked
  /\ finalSeq' = batchSeq
  /\ batchPhase' = "Flushed"
  /\ referenceLogicalWaterline' = ExpectedLogicalWaterline
  /\ referenceBackingWaterlines' = ExpectedBackingWaterlines
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchSeq, currentBacking, currentGeneration, rotationEpoch,
                 captureCount, capturedStamps, expectedStamps,
                 capturedMarkKeys, flushFenceKeys, logicalWaterline,
                 backingWaterlines, publishedLogicalWaterline,
                 publishedBackingWaterlines, plannerPhase, inputRetained,
                 plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

PublishBatch ==
  /\ IF Implementation = "MissingPrepublishFlush"
     THEN /\ batchPhase = "Open"
          /\ captureCount = MaxCaptures
     ELSE batchPhase = "Flushed"
  /\ publishedLogicalWaterline' = logicalWaterline
  /\ publishedBackingWaterlines' = backingWaterlines
  /\ batchPhase' = "Published"
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchSeq, finalSeq, currentBacking, currentGeneration,
                 rotationEpoch, captureCount, capturedStamps, expectedStamps,
                 capturedMarkKeys, flushFenceKeys, logicalWaterline,
                 backingWaterlines, referenceLogicalWaterline,
                 referenceBackingWaterlines, plannerPhase, inputRetained,
                 plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

(* ------------------------------------------------------------------------- *)
(* Immutable preparation and queue-held finalization.                          *)

RetainInput ==
  /\ plannerPhase = "Idle"
  /\ ~inputRetained
  /\ inputRetained' = TRUE
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

PrepareImmutable ==
  /\ plannerPhase = "Idle"
  /\ IF Implementation = "UnretainedInput" THEN TRUE ELSE inputRetained
  /\ plannedPayloadSize' = 2
  /\ plannedEntryValid' = TRUE
  /\ plannedPairCompatible' = TRUE
  /\ plannedDestinationCaptured' =
       (Implementation = "StaleDestination")
  /\ plannedDestinationValid' =
       IF Implementation = "StaleDestination"
       THEN ValidateDestination(queueStopped, arenaActive, directActive)
       ELSE FALSE
  /\ plannerPhase' = "Planned"
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 inputRetained, queueOwner, queueStopped, arenaActive,
                 directActive, destinationValidated, effectApplied,
                 sequenceAssigned, published, publicationWhileQueueHeld>>

AcquireQueue ==
  /\ plannerPhase = "Planned"
  /\ queueOwner = "Free"
  /\ queueOwner' = "Held"
  /\ destinationValidated' = FALSE
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueStopped, arenaActive, directActive, effectApplied,
                 sequenceAssigned, published, publicationWhileQueueHeld>>

InvalidateDestination ==
  /\ plannerPhase = "Planned"
  /\ queueOwner = "Free"
  /\ \E kind \in {"stop", "arena", "direct"} :
       /\ queueStopped' = IF kind = "stop" THEN TRUE ELSE queueStopped
       /\ arenaActive' = IF kind = "arena" THEN TRUE ELSE arenaActive
       /\ directActive' = IF kind = "direct" THEN TRUE ELSE directActive
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueOwner, destinationValidated, effectApplied,
                 sequenceAssigned, published, publicationWhileQueueHeld>>

ValidateDestinationAction ==
  /\ plannerPhase = "Planned"
  /\ queueOwner = "Held"
  /\ ~destinationValidated
  /\ destinationValidated' =
       ValidateDestination(queueStopped, arenaActive, directActive)
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueOwner, queueStopped, arenaActive, directActive,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

ApplyPrepared ==
  /\ plannerPhase = "Planned"
  /\ queueOwner = "Held"
  /\ plannedEntryValid
  /\ plannedPairCompatible
  /\ IF Implementation = "StaleDestination"
     THEN /\ plannedDestinationCaptured
          /\ plannedDestinationValid
     ELSE destinationValidated
  /\ effectApplied' = TRUE
  /\ sequenceAssigned' = TRUE
  /\ published' = TRUE
  /\ publicationWhileQueueHeld' = TRUE
  /\ plannerPhase' = "Applied"
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 inputRetained, plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated>>

RejectPrepared ==
  /\ plannerPhase = "Planned"
  /\ queueOwner = "Held"
  /\ ~destinationValidated
  /\ plannerPhase' = "Rejected"
  /\ queueOwner' = "Free"
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 inputRetained, plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueStopped, arenaActive,
                 directActive, destinationValidated, effectApplied,
                 sequenceAssigned, published, publicationWhileQueueHeld>>

ReleaseQueue ==
  /\ plannerPhase = "Applied"
  /\ queueOwner = "Held"
  /\ queueOwner' = "Free"
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, inputRetained, plannedPayloadSize,
                 plannedEntryValid, plannedPairCompatible,
                 plannedDestinationCaptured, plannedDestinationValid,
                 queueStopped, arenaActive, directActive,
                 destinationValidated, effectApplied, sequenceAssigned,
                 published, publicationWhileQueueHeld>>

ReleaseInput ==
  /\ plannerPhase \in {"Applied", "Rejected"}
  /\ inputRetained
  /\ inputRetained' = FALSE
  /\ UNCHANGED <<arenaOwner, rangePhase, rangeIdentity, rangeGeneration,
                 rangeAdmitted, rangeUploaded, rangeShadow, rangeMetal,
                 batchPhase, batchSeq, finalSeq, currentBacking,
                 currentGeneration, rotationEpoch, captureCount,
                 capturedStamps, expectedStamps, capturedMarkKeys,
                 flushFenceKeys, logicalWaterline, backingWaterlines,
                 referenceLogicalWaterline, referenceBackingWaterlines,
                 publishedLogicalWaterline, publishedBackingWaterlines,
                 plannerPhase, plannedPayloadSize, plannedEntryValid,
                 plannedPairCompatible, plannedDestinationCaptured,
                 plannedDestinationValid, queueOwner, queueStopped,
                 arenaActive, directActive, destinationValidated,
                 effectApplied, sequenceAssigned, published,
                 publicationWhileQueueHeld>>

Next ==
  \/ AcquireRange
  \/ StageRangeBytes
  \/ AdmitRange
  \/ ApplyRange
  \/ ReleaseRange
  \/ BeginBatch
  \/ Capture
  \/ MarkCaptured
  \/ MarkCurrentFence
  \/ RotateBacking
  \/ FlushBatch
  \/ PublishBatch
  \/ RetainInput
  \/ PrepareImmutable
  \/ AcquireQueue
  \/ InvalidateDestination
  \/ ValidateDestinationAction
  \/ ApplyPrepared
  \/ RejectPrepared
  \/ ReleaseQueue
  \/ ReleaseInput

Spec == Init /\ [][Next]_vars /\ WF_vars(FlushBatch)

(* ------------------------------------------------------------------------- *)
(* Safety and progress obligations.                                            *)

TypeOK ==
  /\ arenaOwner \in ArenaOwners
  /\ rangePhase \in RangePhases
  /\ rangeIdentity = LogicalHandle
  /\ rangeGeneration = ActiveGeneration
  /\ rangeAdmitted \in BOOLEAN
  /\ rangeUploaded \in BOOLEAN
  /\ rangeShadow \in [Positions -> Nat]
  /\ rangeMetal \in [Positions -> Nat]
  /\ batchPhase \in BatchPhases
  /\ batchSeq \in Nat
  /\ finalSeq \in Nat
  /\ currentBacking \in Backings
  /\ currentGeneration = ActiveGeneration
  /\ rotationEpoch \in Nat
  /\ captureCount \in 0..MaxCaptures
  /\ capturedStamps \subseteq [logical: {LogicalHandle},
                                  generation: {ActiveGeneration},
                                  backing: Backings]
  /\ expectedStamps \subseteq [logical: {LogicalHandle},
                                  generation: {ActiveGeneration},
                                  backing: Backings]
  /\ capturedMarkKeys \subseteq [kind: {"captured"},
                                  logical: {LogicalHandle},
                                  generation: {ActiveGeneration},
                                  backing: Backings]
  /\ logicalWaterline \in Nat
  /\ backingWaterlines \in [Backings -> Nat]
  /\ referenceLogicalWaterline \in Nat
  /\ referenceBackingWaterlines \in [Backings -> Nat]
  /\ publishedLogicalWaterline \in Nat
  /\ publishedBackingWaterlines \in [Backings -> Nat]
  /\ plannerPhase \in PlannerPhases
  /\ inputRetained \in BOOLEAN
  /\ plannedPayloadSize \in Nat
  /\ plannedEntryValid \in BOOLEAN
  /\ plannedPairCompatible \in BOOLEAN
  /\ plannedDestinationCaptured \in BOOLEAN
  /\ plannedDestinationValid \in BOOLEAN
  /\ queueOwner \in QueueOwners
  /\ queueStopped \in BOOLEAN
  /\ arenaActive \in BOOLEAN
  /\ directActive \in BOOLEAN
  /\ destinationValidated \in BOOLEAN
  /\ effectApplied \in BOOLEAN
  /\ sequenceAssigned \in BOOLEAN
  /\ published \in BOOLEAN
  /\ publicationWhileQueueHeld \in BOOLEAN

RangeRequiresArenaExclusivity ==
  rangePhase \in {"Admitted", "Uploaded"} => arenaOwner = "Range"

RangeIdentityGenerationPreserved ==
  /\ rangeIdentity = LogicalHandle
  /\ rangeGeneration = ActiveGeneration

NoOverwriteReadPreserved ==
  \A p \in ReadPositions : rangeMetal[p] = InitialReadValue

RangeChangesOnlyWriteRange ==
  \A p \in Positions \ WritePositions : rangeMetal[p] =
    IF p \in ReadPositions THEN InitialReadValue ELSE 0

RangeUploadMatchesInput ==
  rangeUploaded => rangeMetal = InputBytes

CapturedIdentityGenerationExact ==
  \A stamp \in capturedStamps :
    /\ stamp.logical = LogicalHandle
    /\ stamp.generation = ActiveGeneration

CapturedBackingExact == capturedStamps = expectedStamps

CapturedMarksDeduplicateExactStamp ==
  /\ \A key \in capturedMarkKeys : key.kind = "captured"
  /\ Cardinality(capturedMarkKeys) <= Cardinality(capturedStamps)

FenceKeysCarryRotationEpoch ==
  \A key \in flushFenceKeys : key.kind \in {"logical", "current-backing"}

BatchSequenceFixed ==
  finalSeq # 0 => finalSeq = batchSeq

WaterlinesUsePureMax ==
  batchPhase \in {"Flushed", "Published"} =>
    /\ logicalWaterline = referenceLogicalWaterline
    /\ backingWaterlines = referenceBackingWaterlines

PublicationRequiresFinalFlush ==
  batchPhase = "Published" =>
    /\ finalSeq = batchSeq
    /\ publishedLogicalWaterline = referenceLogicalWaterline
    /\ publishedBackingWaterlines = referenceBackingWaterlines

PreparationRequiresRetainedInput ==
  plannerPhase = "Planned" => inputRetained

PreparationIsImmutable ==
  plannerPhase = "Planned" =>
    /\ plannedPayloadSize = 2
    /\ plannedEntryValid
    /\ plannedPairCompatible

NoEffectBeforePreparation ==
  effectApplied => plannerPhase = "Applied"

EffectRequiresPostAcquireValidation ==
  effectApplied =>
    /\ queueOwner = "Held" \/ publicationWhileQueueHeld
    /\ destinationValidated
    /\ ValidateDestination(queueStopped, arenaActive, directActive)

QueueSequenceAndPublicationHeld ==
  sequenceAssigned /\ published => publicationWhileQueueHeld

BoundedFlushEventually ==
  (batchPhase = "Open" /\ captureCount = MaxCaptures /\
     AllCapturedMarked /\ CurrentFenceMarked) ~> batchPhase = "Flushed"

====
