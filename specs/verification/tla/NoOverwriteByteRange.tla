---- MODULE NoOverwriteByteRange ----
(******************************************************************************
 * DEFAULT+DYNAMIC D3DLOCK_NOOVERWRITE byte-range publication.
 *
 * The application promises that WriteRange is disjoint from InFlightRead.
 * A production range upload changes only WriteRange.  The deliberately
 * modelled BuggyFullShadowUpload action copies the entire CPU shadow and
 * therefore overwrites the bytes still read by the GPU; the companion
 * counterexample configuration is expected to fail NoOverwriteReadPreserved.
 *******************************************************************************)

EXTENDS Naturals, FiniteSets

CONSTANTS BytePositions, InFlightRead, WriteRange, InitialGpuValue,
          ApplicationValue, Implementation

ASSUME BytePositions # {}
ASSUME InFlightRead \subseteq BytePositions
ASSUME WriteRange \subseteq BytePositions
ASSUME InFlightRead \cap WriteRange = {}
ASSUME InitialGpuValue \in Nat
ASSUME ApplicationValue \in Nat
ASSUME Implementation \in {"Exact", "Buggy"}

VARIABLES shadow, metal

vars == <<shadow, metal>>

InitialShadow == [p \in BytePositions |-> 0]
InitialMetal ==
  [p \in BytePositions |->
    IF p \in InFlightRead THEN InitialGpuValue ELSE 0]

Init ==
  /\ shadow = InitialShadow
  /\ metal = InitialMetal

(* CPU writes are bounded by the application's promised NOOVERWRITE range. *)
ApplicationWrite ==
  /\ shadow' =
       [p \in BytePositions |->
         IF p \in WriteRange THEN ApplicationValue ELSE shadow[p]]
  /\ UNCHANGED metal

(* Production transition: only the written range is copied to Metal. *)
ExactRangeUpload ==
  /\ shadow' = shadow
  /\ metal' =
       [p \in BytePositions |->
         IF p \in WriteRange THEN shadow[p] ELSE metal[p]]

(* Counterexample transition: copying the whole CPU shadow violates the
 * NOOVERWRITE read/write disjointness once the shadow lags GPU contents. *)
BuggyFullShadowUpload ==
  /\ shadow' = shadow
  /\ metal' = shadow

ProductionNext ==
  \/ ApplicationWrite
  \/ ExactRangeUpload

BuggyNext ==
  \/ ApplicationWrite
  \/ BuggyFullShadowUpload

Next == IF Implementation = "Exact" THEN ProductionNext ELSE BuggyNext

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ shadow \in [BytePositions -> Nat]
  /\ metal \in [BytePositions -> Nat]

(* The in-flight GPU read bytes remain unchanged until their sequence retires. *)
NoOverwriteReadPreserved ==
  \A p \in InFlightRead : metal[p] = InitialGpuValue

(* An exact upload cannot mutate any byte outside the app-valid write range. *)
ExactUploadPreservesReadRange ==
  \A p \in BytePositions \ WriteRange :
    metal[p] = InitialMetal[p]

(* The buggy action is intentionally kept as a first-class transition so a
 * TLC run with Implementation = "Buggy" emits a short invariant trace. *)
BuggyCounterexample ==
  Init /\ ApplicationWrite /\ BuggyFullShadowUpload

====
