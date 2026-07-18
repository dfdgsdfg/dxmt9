---- MODULE WireObjectRegistryV2 ----
(*
 * dxmt9 command-chunk V2 device-local wire object registry.
 *
 * A producer captures {slot, generation, kind} while a wrapper is live. The
 * unix importer admits that identity only when all three fields still match.
 * Final release advances the generation before a slot can be reused. A slot
 * at MAX_GENERATION is retired instead of wrapping to an identity that may
 * have existed before.
 *
 * This model deliberately includes forged requests so the checked state space
 * covers stale generations and wrong kinds as well as identities captured from
 * a live wrapper.
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS Slots, Requests, Kinds, MAX_GENERATION

ASSUME Slots # {}
ASSUME Requests # {}
ASSUME Kinds # {}
ASSUME "None" \notin Kinds
ASSUME MAX_GENERATION \in Nat /\ MAX_GENERATION >= 2

SlotStates == {"Free", "Live", "Retired"}
Outcomes == {"Empty", "Pending", "Admitted", "Rejected"}

VARIABLES
  slotState,
  slotGeneration,
  slotKind,
  releasedGeneration,
  requestSlot,
  requestGeneration,
  requestKind,
  outcome,
  observedState,
  observedGeneration,
  observedKind

vars == <<slotState, slotGeneration, slotKind, releasedGeneration,
          requestSlot, requestGeneration, requestKind, outcome,
          observedState, observedGeneration, observedKind>>

InitialKind == CHOOSE k \in Kinds : TRUE

Init ==
  /\ slotState = [s \in Slots |-> "Live"]
  /\ slotGeneration = [s \in Slots |-> 1]
  /\ slotKind = [s \in Slots |-> InitialKind]
  /\ releasedGeneration = [s \in Slots |-> 0]
  /\ requestSlot = [r \in Requests |-> "None"]
  /\ requestGeneration = [r \in Requests |-> 0]
  /\ requestKind = [r \in Requests |-> "None"]
  /\ outcome = [r \in Requests |-> "Empty"]
  /\ observedState = [r \in Requests |-> "Free"]
  /\ observedGeneration = [r \in Requests |-> 0]
  /\ observedKind = [r \in Requests |-> "None"]

CaptureLive(r, s) ==
  /\ outcome[r] = "Empty"
  /\ slotState[s] = "Live"
  /\ requestSlot' = [requestSlot EXCEPT ![r] = s]
  /\ requestGeneration' =
       [requestGeneration EXCEPT ![r] = slotGeneration[s]]
  /\ requestKind' = [requestKind EXCEPT ![r] = slotKind[s]]
  /\ outcome' = [outcome EXCEPT ![r] = "Pending"]
  /\ UNCHANGED <<slotState, slotGeneration, slotKind, releasedGeneration,
                  observedState, observedGeneration, observedKind>>

ForgeRequest(r, s, generation, kind) ==
  /\ outcome[r] = "Empty"
  /\ generation \in 1..MAX_GENERATION
  /\ kind \in Kinds
  /\ requestSlot' = [requestSlot EXCEPT ![r] = s]
  /\ requestGeneration' = [requestGeneration EXCEPT ![r] = generation]
  /\ requestKind' = [requestKind EXCEPT ![r] = kind]
  /\ outcome' = [outcome EXCEPT ![r] = "Pending"]
  /\ UNCHANGED <<slotState, slotGeneration, slotKind, releasedGeneration,
                  observedState, observedGeneration, observedKind>>

Release(s) ==
  /\ slotState[s] = "Live"
  /\ releasedGeneration' =
       [releasedGeneration EXCEPT ![s] = slotGeneration[s]]
  /\ IF slotGeneration[s] = MAX_GENERATION
        THEN /\ slotState' = [slotState EXCEPT ![s] = "Retired"]
             /\ slotGeneration' = slotGeneration
        ELSE /\ slotState' = [slotState EXCEPT ![s] = "Free"]
             /\ slotGeneration' =
                  [slotGeneration EXCEPT ![s] = @ + 1]
  /\ slotKind' = [slotKind EXCEPT ![s] = "None"]
  /\ UNCHANGED <<requestSlot, requestGeneration, requestKind, outcome,
                  observedState, observedGeneration, observedKind>>

Register(s, kind) ==
  /\ slotState[s] = "Free"
  /\ kind \in Kinds
  /\ slotState' = [slotState EXCEPT ![s] = "Live"]
  /\ slotKind' = [slotKind EXCEPT ![s] = kind]
  /\ UNCHANGED <<slotGeneration, releasedGeneration, requestSlot,
                  requestGeneration, requestKind, outcome, observedState,
                  observedGeneration, observedKind>>

Resolve(r) ==
  /\ outcome[r] = "Pending"
  /\ LET s == requestSlot[r]
         matches == /\ slotState[s] = "Live"
                    /\ slotGeneration[s] = requestGeneration[r]
                    /\ slotKind[s] = requestKind[r]
     IN outcome' = [outcome EXCEPT
          ![r] = IF matches THEN "Admitted" ELSE "Rejected"]
  /\ observedState' =
       [observedState EXCEPT ![r] = slotState[requestSlot[r]]]
  /\ observedGeneration' =
       [observedGeneration EXCEPT ![r] = slotGeneration[requestSlot[r]]]
  /\ observedKind' =
       [observedKind EXCEPT ![r] = slotKind[requestSlot[r]]]
  /\ UNCHANGED <<slotState, slotGeneration, slotKind, releasedGeneration,
                  requestSlot, requestGeneration, requestKind>>

Next ==
  \/ \E r \in Requests, s \in Slots : CaptureLive(r, s)
  \/ \E r \in Requests, s \in Slots,
       generation \in 1..MAX_GENERATION, kind \in Kinds :
       ForgeRequest(r, s, generation, kind)
  \/ \E s \in Slots : Release(s)
  \/ \E s \in Slots, kind \in Kinds : Register(s, kind)
  \/ \E r \in Requests : Resolve(r)

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ slotState \in [Slots -> SlotStates]
  /\ slotGeneration \in [Slots -> 1..MAX_GENERATION]
  /\ slotKind \in [Slots -> (Kinds \union {"None"})]
  /\ releasedGeneration \in [Slots -> 0..MAX_GENERATION]
  /\ requestSlot \in [Requests -> (Slots \union {"None"})]
  /\ requestGeneration \in [Requests -> 0..MAX_GENERATION]
  /\ requestKind \in [Requests -> (Kinds \union {"None"})]
  /\ outcome \in [Requests -> Outcomes]
  /\ observedState \in [Requests -> SlotStates]
  /\ observedGeneration \in [Requests -> 0..MAX_GENERATION]
  /\ observedKind \in [Requests -> (Kinds \union {"None"})]

NoZombieAccept ==
  \A r \in Requests :
    outcome[r] = "Admitted" =>
      /\ observedState[r] = "Live"
      /\ observedGeneration[r] = requestGeneration[r]

KindStable ==
  \A r \in Requests :
    outcome[r] = "Admitted" => observedKind[r] = requestKind[r]

NoReuseWithoutGenerationAdvance ==
  \A s \in Slots :
    (releasedGeneration[s] > 0 /\ slotState[s] = "Live") =>
      slotGeneration[s] > releasedGeneration[s]

NoGenerationWrap ==
  \A s \in Slots :
    (releasedGeneration[s] = MAX_GENERATION) =>
      slotState[s] = "Retired"

=============================================================================
