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
(*  3. Retention. The 64 compatibility payloads are PERSISTENT: a          *)
(*     publication reclaims one and `clearCommands()` retains every          *)
(*     vector's capacity. Reprovisioning such a payload frees and            *)
(*     re-allocates storage that is already correct, so a sufficient         *)
(*     retained payload must be adopted in place -- but only when its        *)
(*     retention is COMPLETE in every dimension and the aggregate ledger      *)
(*     entry still describes it exactly (R-BACK-2.105).                      *)
(*                                                                          *)
(*  4. Boundary credits against a same-capacity serial reference. The       *)
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
  \* "Enforced" adopts a reclaimed payload whose retained storage already
  \* covers the plan. "Removed" always re-stages a fresh topology, which is
  \* what HEAD did before R-BACK-2.105.
  ReuseDiscipline,
  \* "Enforced" reclaims a payload with every dimension intact. "Removed"
  \* models the detach that surrendered the `drawShaderLayouts` buffer: the
  \* payload keeps its BYTES but not its shape, so a bytes-only skip rule
  \* would wrongly accept it.
  RetentionDiscipline,
  \* "Enforced" additionally requires the aggregate ledger entry for the
  \* payload to equal its actual retained bytes before reuse may skip the
  \* lease. "Removed" trusts physical coverage alone.
  ReuseQualification,
  \* "Settled" settles the aggregate ledger on every provision.
  \* "DeniedAfterFirst" settles only the first, after which the payload grows
  \* by ordinary exact-fit reservation and the ledger is left behind -- the
  \* production shape where a lease was denied.
  LeaseDiscipline,
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
  provisionedWhilePopulated,
  owner,           \* which persistent payload the writing slot is bound to
  retained,        \* [bytes, complete] surviving reclaim, per payload
  ledgerBytes,     \* aggregate ledger's recorded bytes per payload; 0 = none
  reprovisionedOverSufficient,
  reusedIncompleteRetention,
  staleLedgerReuse

vars == <<srcs, cursor, slotUsed, slotCapacity, slotGeneration, published,
  refUsed, refPublished, grewWhilePopulated, provisionedWhilePopulated,
  owner, retained, ledgerBytes, reprovisionedOverSufficient,
  reusedIncompleteRetention, staleLedgerReuse>>

\* Two payloads are enough to prove the retention SHAPE: a rotation must hand
\* the writing slot to a different persistent payload and later come back to
\* one that still holds its capacity. The production ring is 64 and that count
\* stays a native assertion, not a model parameter.
Payloads == {1, 2}

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
    \* Reaches a payload's THIRD visit, which is the only way a lease denied
    \* after the first provision can leave the aggregate ledger describing
    \* pre-growth bytes while a later, smaller source still finds complete
    \* physical coverage. Visits 1 and 3 of payload 1 are `4` and `1`; visit 2
    \* is the `8` that grows it past what the ledger recorded.
    <<4, 4, 8, 4, 1>>,
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

Max(a, b) == IF a > b THEN a ELSE b

\* --- retention -----------------------------------------------------------
\*
\* `retained[p].bytes` is the physical capacity payload p still owns after a
\* reclaim; `retained[p].complete` is whether every dimension survived it.
\* Production's reclaim keeps both (`clearCommands()` retains capacity, and the
\* detached owner buffer is moved back after the relock); RetentionDiscipline =
\* "Removed" is the shape where one dimension's buffer was surrendered.
\* "Removed" is the shape audited on HEAD, and it is deliberately BOTH halves
\* of that shape at once: reclaim surrenders one dimension's buffer, and the
\* skip rule looks only at the byte total. That pair is what makes a payload
\* which is sufficient by bytes and broken by shape acceptable to reuse.
\*
\* `base` is the retention function AS OF the claim -- the current one on an
\* empty slot, the post-reclaim one on a rotation -- so both arms share one
\* definition instead of restating the policy twice.
Sufficient(base, p, cost) ==
  /\ base[p].bytes >= Provisioned(cost)
  /\ (RetentionDiscipline = "Enforced" => base[p].complete)

\* Physical coverage is not enough to skip the aggregate lease: the ledger
\* entry must still be an exact description of the payload's storage.
LedgerQualified(base, p) ==
  IF ReuseQualification = "Enforced"
    THEN ledgerBytes[p] # 0 /\ ledgerBytes[p] = base[p].bytes
    ELSE TRUE

Reuses(base, p, cost) ==
  /\ ReuseDiscipline = "Enforced"
  /\ Sufficient(base, p, cost)
  /\ LedgerQualified(base, p)

\* Only the first provision on a payload settles the ledger under
\* "DeniedAfterFirst"; a later one is a denied lease that still grows the
\* payload by ordinary exact-fit reservation.
Settles(p) == LeaseDiscipline = "Settled" \/ ledgerBytes[p] = 0

ProvisionedBytes(base, p, cost) ==
  IF Settles(p) THEN Provisioned(cost) ELSE Max(base[p].bytes, cost)

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
  /\ owner = 1
  /\ retained = [p \in Payloads |-> [bytes |-> 0, complete |-> TRUE]]
  /\ ledgerBytes = [p \in Payloads |-> 0]
  /\ reprovisionedOverSufficient = FALSE
  /\ reusedIncompleteRetention = FALSE
  /\ staleLedgerReuse = FALSE

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

\* Claim empty payload `p` for a source of `cost`, starting from retention
\* `base`: adopt the retained storage in place when that is provably sound,
\* else stage a fresh topology. Storage only; neither arm publishes.
ClaimReuse(base, p) ==
  /\ slotCapacity' = base[p].bytes
  /\ retained' = base
  /\ ledgerBytes' = ledgerBytes
  /\ reprovisionedOverSufficient' = reprovisionedOverSufficient
  \* Witnesses for the two premises a coverage-only rule would drop.
  \* Parenthesised deliberately: `=` binds tighter than `\/`, so
  \* `x' = x \/ e` would parse as `(x' = x) \/ e` and TLC could satisfy the
  \* step through `e` without ever assigning `x'`.
  /\ reusedIncompleteRetention' = (reusedIncompleteRetention \/ ~base[p].complete)
  /\ staleLedgerReuse' =
       (staleLedgerReuse \/ ledgerBytes[p] # base[p].bytes)

ClaimProvision(base, p, cost) ==
  /\ slotCapacity' = ProvisionedBytes(base, p, cost)
  /\ retained' = [base EXCEPT
       ![p] = [bytes |-> ProvisionedBytes(base, p, cost), complete |-> TRUE]]
  /\ ledgerBytes' = [ledgerBytes EXCEPT ![p] =
       IF Settles(p) THEN ProvisionedBytes(base, p, cost) ELSE ledgerBytes[p]]
  \* Freeing and re-allocating storage the payload already held, and held in a
  \* shape and with a ledger that would have licensed adopting it in place.
  /\ reprovisionedOverSufficient' =
       (reprovisionedOverSufficient \/
        (base[p].bytes >= Provisioned(cost) /\ base[p].complete /\
         LedgerQualified(base, p)))
  /\ reusedIncompleteRetention' = reusedIncompleteRetention
  /\ staleLedgerReuse' = staleLedgerReuse

ClaimEmpty(base, p, cost) ==
  IF Reuses(base, p, cost) THEN ClaimReuse(base, p)
                           ELSE ClaimProvision(base, p, cost)

\* Empty slot: claim its payload, then construct. This is the only transition
\* that may change physical capacity.
ProvisionEmpty ==
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed = 0
  /\ ClaimEmpty(retained, owner, Cost(cursor))
  /\ slotUsed' = Cost(cursor)
  /\ cursor' = cursor + 1
  /\ published' = published
  /\ slotGeneration' = slotGeneration
  /\ owner' = owner
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
      provisionedWhilePopulated, owner, retained, ledgerBytes,
      reprovisionedOverSufficient, reusedIncompleteRetention,
      staleLedgerReuse>>
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
  /\ retained' = [retained EXCEPT
       ![owner] = [bytes |-> slotUsed + Cost(cursor), complete |-> TRUE]]
  /\ UNCHANGED <<slotGeneration, published, owner, ledgerBytes,
      reprovisionedOverSufficient, reusedIncompleteRetention,
      staleLedgerReuse>>
  /\ RefStep(Cost(cursor))

\* Populated slot without room: publish the extent, reclaim the payload it was
\* bound to, and bind the next persistent payload. This is one admission in
\* production -- `beginDirectChunkSlotReplay` rotates and retries the same plan
\* under one bounded rotation -- so the publication and the source's
\* consumption are one step here too, and the reference is charged in the same
\* step rather than one state later.
\*
\* Reclaim is part of THIS step, not a separate nondeterministic action: in
\* production the payload is reclaimed by the queue lifecycle and cannot be
\* re-admitted before that completes, so interleaving it against every cursor
\* position would model concurrency the ring does not have.
Reclaimed(p) ==
  [retained EXCEPT
     ![p] = [bytes |-> @.bytes,
             complete |-> RetentionDiscipline = "Enforced"]]

RotateAndProvision ==
  /\ ~Done
  /\ UNCHANGED srcs
  /\ slotUsed > 0
  /\ slotUsed + Cost(cursor) > slotCapacity
  /\ published' = published + 1
  /\ slotGeneration' = slotGeneration + 1
  /\ owner' = (owner % 2) + 1
  \* The outgoing payload is reclaimed in this same step, and the incoming one
  \* is claimed from the post-reclaim retention. Reclaim touches only the
  \* outgoing payload, so the two commute and the shared `ClaimEmpty` applies
  \* unchanged.
  /\ ClaimEmpty(Reclaimed(owner), (owner % 2) + 1, Cost(cursor))
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
  /\ owner \in Payloads
  /\ retained \in [Payloads -> [bytes: 0..MaxCost, complete: BOOLEAN]]
  /\ ledgerBytes \in [Payloads -> 0..MaxCost]
  /\ reprovisionedOverSufficient \in BOOLEAN
  /\ reusedIncompleteRetention \in BOOLEAN
  /\ staleLedgerReuse \in BOOLEAN

\* Physical capacity is set only while the slot is empty.
ProvisionOnlyWhenEmpty == provisionedWhilePopulated = FALSE

\* A populated slot is never reallocated, rehashed or copied.
NoGrowWhilePopulated == grewWhilePopulated = FALSE

\* Reservation is never under-provisioned: everything constructed fits.
CapacityBoundsConstruction == slotUsed <= slotCapacity

\* Storage rotation must not add a Metal boundary the same-capacity serial
\* reference did not have.
BoundaryCreditsNotExceeded == published <= refPublished

\* R-BACK-2.105. A payload whose retained storage already covers the plan is
\* never freed and re-allocated.
NoRedundantReprovision == reprovisionedOverSufficient = FALSE

\* Reuse is per-dimension, not bytes-only: a payload that lost one dimension to
\* reclaim must not be adopted in place on the strength of its byte total.
ReuseRequiresCompleteRetention == reusedIncompleteRetention = FALSE

\* Reuse skips the aggregate lease, so it may only be taken when the ledger
\* entry is still an exact description of the payload's storage.
ReuseIsLedgerQualified == staleLedgerReuse = FALSE

Spec == Init /\ [][Next]_vars

====
