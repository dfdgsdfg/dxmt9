---- MODULE QueryOcclusionResults ----
EXTENDS Naturals, FiniteSets, TLC

(******************************************************************************
 * Bounded GPU occlusion result lifecycle.
 *
 * A BEGIN allocates an issue-qualified result generation.  Each render pass
 * freezes the active membership set and owns one independent contribution;
 * the completion callback credits every frozen result exactly once.  END
 * publication/submission/GPU completion are intentionally separate from the
 * result's pending callback credits.  Release is terminal for the bounded
 * public object: its issue ordinal remains monotone and no later BEGIN can
 * reuse the released object's identity.
 *
 * Conceptual production bindings (not a Metal proof):
 *   - include/dxmt9/query_occlusion.hpp: OcclusionResult::addSaturated,
 *     OcclusionResult::readableAfterFence, OcclusionScope's immutable 64-slot
 *     membership snapshot;
 *   - src/d3d9/core_resources.cpp: Device::getQueryData and issueQuery's
 *     issue-qualified result/reissue and END receipt path;
 *   - src/dxmt9/dxmt9_command_queue.cpp: setOcclusionScope freezes a source
 *     boundary and prepareSlotForPublish carries that frozen scope;
 *   - src/dxmt9/dxmt9_draw_encoder_*: render-pass callback adds one pass
 *     contribution to every frozen result and decrements pending once.
 *
 * The model excludes Metal buffer bytes, Objective-C memory ordering, driver
 * visibility semantics, and final pixels.  Its safety obligations are the
 * lifecycle/identity/refinement gates that precede those concrete oracles.
 ******************************************************************************)

CONSTANTS Queries, MaxGeneration, MaxPasses, MaxPending, Mutation

Generation == 1..MaxGeneration
KeySet == Queries \X Generation
PassSet == 1..MaxPasses
PassStates == {"Unused", "Published", "Submitted", "Completed"}

VARIABLES issue, released, active, passState, passScope, passValue,
          callbackDone, callbackTargets, pending, samples, failedResult,
          endPass, reclaimed, queueFailed,
          ok, observedKey, observedPass, observedPending,
          observedPassCompleted, observedFailed

vars == <<issue, released, active, passState, passScope, passValue,
           callbackDone, callbackTargets, pending, samples, failedResult,
           endPass, reclaimed, queueFailed,
           ok, observedKey, observedPass, observedPending,
           observedPassCompleted, observedFailed>>

CurrentKeys == {k \in KeySet:
  issue[k[1]] = k[2] /\ k[2] # 0 /\ k[1] \notin released}

Init ==
  /\ issue = [q \in Queries |-> 0]
  /\ released = {}
  /\ active = {}
  /\ passState = [p \in PassSet |-> "Unused"]
  /\ passScope = [p \in PassSet |-> {}]
  /\ passValue = [p \in PassSet |-> 0]
  /\ callbackDone = [p \in PassSet |-> FALSE]
  /\ callbackTargets = [p \in PassSet |-> {}]
  /\ pending = [k \in KeySet |-> 0]
  /\ samples = [k \in KeySet |-> 0]
  /\ failedResult = [k \in KeySet |-> FALSE]
  /\ endPass = [k \in KeySet |-> 0]
  /\ reclaimed = [k \in KeySet |-> FALSE]
  /\ queueFailed = FALSE
  /\ ok = FALSE
  /\ observedKey = <<0, 0>>
  /\ observedPass = 0
  /\ observedPending = 0
  /\ observedPassCompleted = FALSE
  /\ observedFailed = FALSE

NoObservation ==
  UNCHANGED <<ok, observedKey, observedPass, observedPending,
               observedPassCompleted, observedFailed>>

Begin(q) ==
  LET newKey == <<q, issue[q] + 1>>
      oldKey == IF issue[q] = 0 THEN {} ELSE {<<q, issue[q]>>}
  IN
    /\ ~queueFailed
    /\ q \notin released
    /\ issue[q] < MaxGeneration
    /\ issue' = [issue EXCEPT ![q] = issue[q] + 1]
    /\ active' = (active \ oldKey) \cup {newKey}
    /\ UNCHANGED <<released, passState, passScope, passValue, callbackDone,
                   callbackTargets, pending, samples, failedResult, endPass,
                   reclaimed, queueFailed>>
    /\ NoObservation

End(q) ==
  LET k == <<q, issue[q]>>
      p == CHOOSE candidate \in PassSet:
             passState[candidate] = "Unused"
      nextPending == [x \in KeySet |->
        pending[x] + IF x \in active THEN 1 ELSE 0]
  IN
    /\ ~queueFailed
    /\ q \notin released
    /\ issue[q] # 0
    /\ k \in active
    /\ \E candidate \in PassSet: passState[candidate] = "Unused"
    /\ \A x \in active: pending[x] < MaxPending
    /\ \A x \in active: ~reclaimed[x]
    /\ passState' = [passState EXCEPT ![p] = "Published"]
    /\ passScope' = [passScope EXCEPT ![p] = active]
    /\ passValue' = [passValue EXCEPT ![p] = 1]
    /\ callbackDone' = [callbackDone EXCEPT ![p] = FALSE]
    /\ callbackTargets' = [callbackTargets EXCEPT ![p] = {}]
    /\ pending' = nextPending
    /\ endPass' = [endPass EXCEPT ![k] = p]
    /\ active' = active \ {k}
    /\ UNCHANGED <<issue, released, samples, failedResult, reclaimed,
                   queueFailed>>
    /\ NoObservation

Submit(p) ==
  /\ ~queueFailed
  /\ passState[p] = "Published"
  /\ passState' = [passState EXCEPT ![p] = "Submitted"]
  /\ UNCHANGED <<issue, released, active, passScope, passValue, callbackDone,
                 callbackTargets, pending, samples, failedResult, endPass,
                 reclaimed, queueFailed>>
  /\ NoObservation

Complete(p) ==
  /\ ~queueFailed
  /\ passState[p] = "Submitted"
  /\ passState' = [passState EXCEPT ![p] = "Completed"]
  /\ UNCHANGED <<issue, released, active, passScope, passValue, callbackDone,
                 callbackTargets, pending, samples, failedResult, endPass,
                 reclaimed, queueFailed>>
  /\ NoObservation

CallbackTarget(p) ==
  IF Mutation = "RedirectStaleGeneration"
  THEN CurrentKeys
  ELSE passScope[p]

Callback(p) ==
  LET target == CallbackTarget(p)
      nextSamples == [x \in KeySet |->
        IF x \in target
        THEN IF Mutation = "OverwriteContribution"
             THEN passValue[p]
             ELSE samples[x] + passValue[p]
        ELSE samples[x]]
      nextPending == [x \in KeySet |->
        pending[x] - IF x \in passScope[p] THEN 1 ELSE 0]
  IN
    /\ passState[p] = "Completed" \/
       (Mutation = "ReadBeforeFence" /\ passState[p] # "Unused")
    /\ ~callbackDone[p]
    /\ \A x \in passScope[p]: pending[x] > 0
    /\ \A x \in passScope[p]: ~reclaimed[x]
    /\ \A x \in target: ~reclaimed[x]
    /\ callbackDone' = [callbackDone EXCEPT ![p] = TRUE]
    /\ callbackTargets' = [callbackTargets EXCEPT ![p] = target]
    /\ pending' = nextPending
    /\ samples' = nextSamples
    /\ UNCHANGED <<issue, released, active, passState, passScope, passValue,
                   failedResult, endPass, reclaimed, queueFailed>>

    /\ NoObservation

Fail ==
  /\ ~queueFailed
  /\ queueFailed' = TRUE
  /\ failedResult' = [x \in KeySet |->
       IF issue[x[1]] = x[2] THEN TRUE ELSE failedResult[x]]
  /\ UNCHANGED <<issue, released, active, passState, passScope, passValue,
                 callbackDone, callbackTargets, pending, samples, endPass,
                 reclaimed>>
  /\ NoObservation

Poll(q) ==
  LET k == <<q, issue[q]>>
      p == endPass[k]
      fenceOK == Mutation = "ReadBeforeFence" \/ passState[p] = "Completed"
      failureOK == Mutation = "IgnoreFailure" \/
                   (~queueFailed /\ ~failedResult[k])
  IN
    /\ q \notin released
    /\ issue[q] # 0
    /\ endPass[k] # 0
    /\ ~reclaimed[k]
    /\ ok' = (fenceOK /\ pending[k] = 0 /\ failureOK)
    /\ observedKey' = k
    /\ observedPass' = p
    /\ observedPending' = pending[k]
    /\ observedPassCompleted' = (passState[p] = "Completed")
    /\ observedFailed' = (queueFailed \/ failedResult[k])
    /\ UNCHANGED <<issue, released, active, passState, passScope, passValue,
                   callbackDone, callbackTargets, pending, samples,
                   failedResult, endPass, reclaimed, queueFailed>>

Release(q) ==
  LET k == <<q, issue[q]>>
  IN
    /\ q \notin released
    /\ issue[q] # 0
    /\ released' = released \cup {q}
    /\ active' = active \ {k}
    /\ UNCHANGED <<issue, passState, passScope, passValue, callbackDone,
                   callbackTargets, pending, samples, failedResult, endPass,
                   reclaimed,
                   queueFailed>>
    /\ NoObservation

Reclaim(k) ==
  /\ ~reclaimed[k]
  /\ k \notin active
  /\ pending[k] = 0
  /\ \A p \in PassSet:
       ~(passState[p] # "Unused" /\ k \in passScope[p] /\ ~callbackDone[p])
  /\ reclaimed' = [reclaimed EXCEPT ![k] = TRUE]
  /\ UNCHANGED <<issue, active, passState, passScope, passValue,
                 callbackDone, callbackTargets, pending, samples,
                 failedResult, endPass, released, queueFailed>>
  /\ NoObservation

Next ==
  (\E q \in Queries: Begin(q) \/ End(q) \/ Poll(q) \/ Release(q))
  \/ (\E p \in PassSet: Submit(p) \/ Complete(p) \/ Callback(p))
  \/ (\E k \in KeySet: Reclaim(k))
  \/ Fail

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ issue \in [Queries -> 0..MaxGeneration]
  /\ released \subseteq Queries
  /\ active \subseteq KeySet
  /\ passState \in [PassSet -> PassStates]
  /\ passScope \in [PassSet -> SUBSET KeySet]
  /\ passValue \in [PassSet -> 0..1]
  /\ callbackDone \in [PassSet -> BOOLEAN]
  /\ callbackTargets \in [PassSet -> SUBSET KeySet]
  /\ pending \in [KeySet -> 0..MaxPending]
  /\ samples \in [KeySet -> 0..MaxPasses]
  /\ failedResult \in [KeySet -> BOOLEAN]
  /\ endPass \in [KeySet -> 0..MaxPasses]
  /\ reclaimed \in [KeySet -> BOOLEAN]
  /\ queueFailed \in BOOLEAN
  /\ ok \in BOOLEAN
  /\ observedKey \in (KeySet \cup {<<0, 0>>})
  /\ observedPass \in 0..MaxPasses
  /\ observedPending \in 0..MaxPending
  /\ observedPassCompleted \in BOOLEAN
  /\ observedFailed \in BOOLEAN

IssueQualified ==
  \A q \in Queries:
    issue[q] # 0 => <<q, issue[q]>> \in KeySet

ReleasedIdentityStable ==
  \A q \in released: issue[q] # 0

ReleasedNotActive ==
  \A k \in active: k[1] \notin released

ScopeIdentity ==
  \A p \in PassSet: callbackDone[p] => callbackTargets[p] = passScope[p]

IndependentContributions ==
  \A k \in KeySet:
    samples[k] = Cardinality({p \in PassSet:
      callbackDone[p] /\ k \in callbackTargets[p]})

PendingCredits ==
  \A k \in KeySet:
    pending[k] = Cardinality({p \in PassSet:
      passState[p] # "Unused" /\ k \in passScope[p] /\ ~callbackDone[p]})

NoReadBeforeGpu == ok => observedPassCompleted
SuccessHasNoPending == ok => observedPending = 0
FailedNeverSuccess == ok => ~observedFailed

NoStaleWrite ==
  \A p \in PassSet: callbackDone[p] => passScope[p] = callbackTargets[p]

NoLiveWriteAfterReclaim ==
  \A k \in KeySet: reclaimed[k] =>
    pending[k] = 0 /\
    \A p \in PassSet:
      ~(passState[p] # "Unused" /\ k \in passScope[p] /\ ~callbackDone[p])

====
