---- MODULE DirectSlotCapacityProvisioning ----
(***************************************************************************)
(* R-BACK-2.104 empty-slot storage provisioning for direct final-slot      *)
(* lease spans.                                                            *)
(*                                                                         *)
(* `DirectChunkSlotContinuation` models the admission of ONE source onto a  *)
(* populated slot and treats capacity as an opaque boolean. That is exactly *)
(* the dimension that regressed: the assembler reserved `size() + extra`    *)
(* exactly, so every populated slot satisfied `size() = capacity()` and     *)
(* the capacity premise was false by construction for every adjacent        *)
(* source. Each one then had to publish the slot and take a fresh one, and  *)
(* an allocation decision became a Metal command-buffer and render-pass     *)
(* boundary.                                                                *)
(*                                                                          *)
(* This model makes capacity a number and adds the three things the         *)
(* boolean could not express:                                               *)
(*                                                                          *)
(*  1. Provisioning. A slot's physical capacity is set once, while the slot *)
(*     is empty, to a BUDGET-FIXED amount that does not depend on which     *)
(*     source happened to arrive first -- exactly the rule                  *)
(*     `directSlotProvisionDrawBudget` implements. A populated slot is      *)
(*     never grown, rehashed or copied.                                     *)
(*                                                                          *)
(*  2. Slot generation. A rotation publishes the extent and moves to a      *)
(*     fresh slot with a new generation, so nothing carries across a        *)
(*     publication boundary.                                                *)
(*                                                                          *)
(*  3. Boundary credits against a same-capacity serial reference. The       *)
(*     reference consumes the identical source sequence with the identical  *)
(*     budget. Direct must never publish more often than it does: any extra *)
(*     publication is a command buffer and a render pass the reference did  *)
(*     not have.                                                            *)
(*                                                                          *)
(* Scope, stated so it is not over-read: the reference here is a            *)
(* same-capacity serial slot, NOT the production Legacy lane, whose chunk   *)
(* command limit is unbounded by default. Direct's budget is finite, so the *)
(* no-extra-boundary property is proven only relative to an equally bounded *)
(* reference; parity with an unbounded reference is an open obligation      *)
(* recorded in specs/backend/encode-scheduling/gap.md.                      *)
(*                                                                          *)
(* Out of scope, as always: this proves nothing about Metal driver          *)
(* behaviour, resource contents, or final pixels.                           *)
(***************************************************************************)

EXTENDS Naturals, Sequences, TLC

CONSTANTS
  \* "Enforced" provisions an empty slot to the budget. "Removed" restores
  \* exact-fit reservation, i.e. the measured regression.
  ProvisionDiscipline,
  \* "Enforced" allows provisioning only while the slot is empty. "Removed"
  \* lets a populated slot be re-provisioned, which reallocates a published
  \* extent.
  EmptyOnlyDiscipline,
  \* Per-slot storage budget, in source-cost units.
  Budget

VARIABLES
  srcs,            \* the ordered adjacent source sequence under check
  cursor,          \* index of the next source, 1..Len(srcs)+1
  slotUsed,        \* cost already constructed into the current slot
  slotCapacity,    \* physical capacity provisioned for the current slot
  slotGeneration,  \* advances on every publication
  published,       \* Direct publications (command-buffer/pass boundaries)
  refUsed,         \* same-capacity serial reference
  refPublished,
  grewWhilePopulated,
  provisionedWhilePopulated

vars == <<srcs, cursor, slotUsed, slotCapacity, slotGeneration, published,
  refUsed, refPublished, grewWhilePopulated, provisionedWhilePopulated>>

\* The ordered adjacent source sequences under check, each cost positive.
\* Defined in the module rather than as a CONSTANT because a TLC configuration
\* file cannot carry tuple literals.
\*
\* This is a SET, chosen nondeterministically at Init, not one literal: a single
\* deterministic trace would be a simulation, and in particular could not
\* express the two shapes a first-span-proportional sizing rule got wrong. The
\* set therefore covers, at Budget = 4:
\*
\*   - homogeneous small sources that must share one slot;
\*   - a TINY first span followed by budget-sized ones (a proportional rule
\*     sizes the whole slot from the short leading span and rotates every
\*     source after it);
\*   - a LARGE first span above the budget followed by small ones (a
\*     proportional rule reserves it exactly, restoring exact fit);
\*   - sources at and beyond the budget, which are genuine budget rotations
\*     in both lanes and must stay legitimate.
SourceSequences ==
  { <<1, 2, 1, 3, 1, 1>>,
    <<1, 1, 1, 1, 1, 1>>,
    <<1, 4, 4, 4>>,
    <<1, 1, 4, 1, 4>>,
    <<4, 1, 4, 1>>,
    <<5, 1, 1, 1>>,
    <<8, 4, 1>>,
    <<2, 8, 1>>,
    <<3, 3, 3, 3>> }

MaxSourceLength == 6

Cost(i) == srcs[i]
Done == cursor > Len(srcs)

\* A source larger than the budget is its own budget: provisioning must never
\* under-reserve, so the slot is reserved exactly and owns nothing else.
Provisioned(cost) ==
  IF ProvisionDiscipline = "Enforced"
    THEN IF cost > Budget THEN cost ELSE Budget
    ELSE cost

Init ==
  /\ srcs \in SourceSequences
  /\ cursor = 1
  /\ slotUsed = 0
  /\ slotCapacity = 0
  /\ slotGeneration = 0
  /\ published = 0
  /\ refUsed = 0
  /\ refPublished = 0
  /\ grewWhilePopulated = FALSE
  /\ provisionedWhilePopulated = FALSE

\* The reference: one slot whose capacity is the same budget, filled serially,
\* publishing only when the next source does not fit.
RefStep(cost) ==
  IF refUsed + cost <= (IF cost > Budget THEN cost ELSE Budget) /\ refUsed > 0
    THEN /\ refUsed' = refUsed + cost
         /\ refPublished' = refPublished
    ELSE IF refUsed = 0
      THEN /\ refUsed' = cost
           /\ refPublished' = refPublished
      ELSE /\ refUsed' = cost
           /\ refPublished' = refPublished + 1

\* Empty slot: provision once, then construct. This is the only transition
\* that may change physical capacity.
ProvisionEmpty ==
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed = 0
  /\ slotCapacity' = Provisioned(Cost(cursor))
  /\ slotUsed' = Cost(cursor)
  /\ cursor' = cursor + 1
  /\ published' = published
  /\ slotGeneration' = slotGeneration
  /\ grewWhilePopulated' = grewWhilePopulated
  /\ provisionedWhilePopulated' = provisionedWhilePopulated
  /\ RefStep(Cost(cursor))

\* Populated slot with room: append in place. No allocation at all.
AppendInPlace ==
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed > 0
  /\ slotUsed + Cost(cursor) <= slotCapacity
  /\ slotUsed' = slotUsed + Cost(cursor)
  /\ cursor' = cursor + 1
  /\ UNCHANGED <<slotCapacity, slotGeneration, published, grewWhilePopulated,
      provisionedWhilePopulated>>
  /\ RefStep(Cost(cursor))

\* The discipline this model exists to break on demand: re-provisioning a
\* populated slot. It buys capacity by reallocating a published extent.
\* `RotateAndProvision` stays enabled alongside it, so when the empty-only
\* premise is removed the model CHOOSES to grow rather than being forced to --
\* a strictly stronger discrimination than making growth the only move.
GrowWhilePopulated ==
  /\ EmptyOnlyDiscipline = "Removed"
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed > 0
  /\ slotUsed + Cost(cursor) > slotCapacity
  /\ slotCapacity' = slotUsed + Cost(cursor)
  /\ slotUsed' = slotUsed + Cost(cursor)
  /\ cursor' = cursor + 1
  /\ grewWhilePopulated' = TRUE
  /\ provisionedWhilePopulated' = TRUE
  /\ UNCHANGED <<slotGeneration, published>>
  /\ RefStep(Cost(cursor))

\* Populated slot without room: publish the extent and take a fresh slot with a
\* new generation, then provision and construct into it. This is one admission
\* in production -- `beginDirectChunkSlotReplay` rotates and retries the same
\* plan under one bounded rotation -- so the publication and the source's
\* consumption are one step here too, and the reference is charged in the same
\* step rather than one state later.
RotateAndProvision ==
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed > 0
  /\ slotUsed + Cost(cursor) > slotCapacity
  /\ published' = published + 1
  /\ slotGeneration' = slotGeneration + 1
  /\ slotCapacity' = Provisioned(Cost(cursor))
  /\ slotUsed' = Cost(cursor)
  /\ cursor' = cursor + 1
  /\ grewWhilePopulated' = grewWhilePopulated
  /\ provisionedWhilePopulated' = provisionedWhilePopulated
  /\ RefStep(Cost(cursor))

\* The final publication is the semantic one (Present); both lanes take it, so
\* it is not a credit either way.
Finish ==
  /\ Done
  /\ UNCHANGED vars

Next == ProvisionEmpty \/ AppendInPlace \/ GrowWhilePopulated
         \/ RotateAndProvision \/ Finish

MaxCost == Budget * 4
TypeOK ==
  /\ srcs \in SourceSequences
  /\ cursor \in 1..(MaxSourceLength + 1)
  /\ slotUsed \in 0..MaxCost
  /\ slotCapacity \in 0..MaxCost
  /\ slotGeneration \in 0..MaxSourceLength
  /\ published \in 0..MaxSourceLength
  /\ refUsed \in 0..MaxCost
  /\ refPublished \in 0..MaxSourceLength
  /\ grewWhilePopulated \in BOOLEAN
  /\ provisionedWhilePopulated \in BOOLEAN

\* Physical capacity is set only while the slot is empty.
ProvisionOnlyWhenEmpty == provisionedWhilePopulated = FALSE

\* A populated slot is never reallocated, rehashed or copied.
NoGrowWhilePopulated == grewWhilePopulated = FALSE

\* Reservation is never under-provisioned: everything constructed fits.
CapacityBoundsConstruction == slotUsed <= slotCapacity

\* Storage rotation must not add a Metal boundary the same-capacity serial
\* reference did not have.
BoundaryCreditsNotExceeded == published <= refPublished

Spec == Init /\ [][Next]_vars

====
