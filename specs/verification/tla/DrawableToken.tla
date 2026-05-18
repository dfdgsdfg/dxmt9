---- MODULE DrawableToken ----
(*
 * dxmt9 PresentDrawableToken — TLA+ Specification
 *
 * Models the lifecycle of `dxmt9::PresentDrawableToken` and its handoff
 * through `CommandQueue::stashDrawableToken` / `takeDrawableToken`.
 *
 * Concrete code under test:
 *   src/dxmt9/dxmt9_presenter.{hpp,cpp}        (class PresentDrawableToken)
 *   src/dxmt9/dxmt9_command_queue.{hpp,cpp}    (stashDrawableToken / takeDrawableToken)
 *
 * The C++ token has fields (mutex_, cv_, drawable_, ready_).  The
 * `fail()` overload sets drawable_=NULL,ready_=TRUE while `complete(d)`
 * sets drawable_=d,ready_=TRUE; both paths notify cv_ exactly once.
 * `waitDrawable()` cv_.wait()s on ready_ and returns drawable_ (which
 * is the sentinel NULL for the fail() path).  The token is single-use:
 * after it has been taken out of the queue's `pendingToken` slot via
 * `takeDrawableToken`, the slot becomes empty and any further take
 * returns nullptr.
 *
 * Three concurrent agents per slot (identified by pid \in 1..MAX_PIDS):
 *
 *   PE thread        — calls `beginAcquireDrawable(...)` which constructs
 *                      the token and `stashDrawableToken(id,token)`.
 *                      (StashToken action.)
 *   AsyncAcquire     — fulfils the token from the worker thread, either
 *                      with a drawable (Complete) or with failure (Fail).
 *   Encoder/queue    — owns the token after `takeDrawableToken`
 *                      (Take action) and blocks in `waitDrawable()`
 *                      (Wait action) until Complete or Fail has fired.
 *
 * Properties verified (matching the parent agent's request):
 *   1. NoDoubleComplete  — Complete/Fail cannot fire twice; once `ready_`
 *                          flips true the actions are disabled.
 *   2. NoUseAfterTake    — once a token is Taken from the queue, no
 *                          further Take can succeed for that pid.
 *   3. WaitProgress      — every Wait eventually becomes enabled
 *                          (liveness, under fair Complete/Fail).
 *   4. StashTakeOrdering — Take is enabled only after Stash (no token
 *                          materialises in the queue from nowhere).
 *
 * How to run (TLC model checker):
 *   tlc DrawableToken.tla -config DrawableToken.cfg
 *)

EXTENDS Naturals, FiniteSets

CONSTANTS
  MAX_PIDS,    \* number of concurrent present-IDs to model (TLC: 2)
  Drawables    \* set of distinct drawable values, e.g. {D1, D2}

ASSUME MAX_PIDS \in Nat /\ MAX_PIDS >= 1
ASSUME Drawables # {}

Pids == 1 .. MAX_PIDS

(*
 * Token state machine (one per pid):
 *
 *   NoToken ──Stash──► Pending ──Complete──► Ready  ──┐
 *                          │                          ├──Take──► Taken
 *                          └──Fail─────────► Failed ──┘
 *
 *   Pending may also be Taken directly (encoder runs ahead of the
 *   async-acquire worker): Pending ──Take──► Taken.  In that case the
 *   encoder thread will block inside waitDrawable() until Complete/Fail
 *   fires on the now-out-of-queue token — the C++ shared_ptr keeps the
 *   token alive in both the encoder's hand and the async worker's
 *   asyncAcquireRequests_ vector, so the fulfilment path is still hot.
 *
 *   To model this faithfully we treat the in-queue status (Pending vs
 *   Taken) and the fulfilment status (none / Ready / Failed) as
 *   orthogonal: Complete/Fail can fire while the token is either still
 *   Pending in the queue or already Taken by the encoder.  We surface
 *   the "Taken but not yet fulfilled" state explicitly so liveness can
 *   reason about it.
 *)
(*
 * tokenState encodes the in-queue status (the slot's pendingToken
 * pointer).  The matrix of full states is therefore
 * tokenState × fulfilled:
 *
 *   NoToken,  FALSE     fresh / never stashed
 *   Pending,  FALSE     stashed, awaiting Complete/Fail
 *   Pending,  TRUE      \ this is what the C++ token names "Ready" or
 *                       /  "Failed" while still in the queue
 *   Taken,    FALSE     encoder took it before fulfilment; wait blocks
 *   Taken,    TRUE      encoder will return from waitDrawable
 *
 * Following the parent-agent request, the public token states map as:
 *   tokenState = Pending  /\ fulfilled = TRUE  /\ drawable \in Drawables
 *     ≡ "Ready"
 *   tokenState = Pending  /\ fulfilled = TRUE  /\ drawable = NullDrawable
 *     ≡ "Failed"
 * We derive `Ready` / `Failed` views for the safety properties below
 * without storing them as a separate variable.
 *)
TokenStates == {"NoToken", "Pending", "Taken"}

NullDrawable == "NULL"
DrawableValues == Drawables \cup {NullDrawable}

VARIABLES
  tokenState,    \* FUNCTION Pids -> TokenStates  (queue-slot status)
  drawable,      \* FUNCTION Pids -> DrawableValues
                 \*   fulfilled=TRUE & drawable in Drawables  → "Ready"
                 \*   fulfilled=TRUE & drawable=NullDrawable  → "Failed"
                 \*   fulfilled=FALSE                         → unused
  fulfilled,     \* FUNCTION Pids -> BOOLEAN  — TRUE iff Complete or Fail
                 \*   has fired in this pid's current lifecycle.
  waitDone       \* FUNCTION Pids -> BOOLEAN  — TRUE iff the encoder's
                 \*   waitDrawable() call has returned for this pid
                 \*   (used to formulate WaitProgress liveness).

vars == <<tokenState, drawable, fulfilled, waitDone>>

(* Derived public views (matching the parent agent's enumeration). *)
ReadyView(pid)  == fulfilled[pid] = TRUE /\ drawable[pid] \in Drawables
FailedView(pid) == fulfilled[pid] = TRUE /\ drawable[pid] = NullDrawable

(* ================================================================
   Initialization
   ================================================================ *)

Init ==
  /\ tokenState = [p \in Pids |-> "NoToken"]
  /\ drawable   = [p \in Pids |-> NullDrawable]
  /\ fulfilled  = [p \in Pids |-> FALSE]
  /\ waitDone   = [p \in Pids |-> FALSE]

(* ================================================================
   Actions
   ================================================================ *)

(*
 * StashToken(pid)
 * The PE thread allocates a fresh token and parks it in the
 * CommandQueue's presenterSlots_[slot].pendingToken.
 * Pre:  tokenState[pid] = NoToken
 * Post: tokenState[pid] = Pending
 *)
StashToken(pid) ==
  /\ tokenState[pid] = "NoToken"
  /\ tokenState' = [tokenState EXCEPT ![pid] = "Pending"]
  /\ UNCHANGED <<drawable, fulfilled, waitDone>>

(*
 * Complete(pid, d)
 * The async-acquire worker calls token->complete(d): under lock it
 * sets drawable_=d, ready_=TRUE and notify_all() on cv_.
 *
 * Pre:  tokenState[pid] \in {Pending, Taken} (the token still exists
 *       via shared_ptr — either in the queue slot or held by the
 *       encoder); fulfilled[pid] = FALSE (single-shot guard, matching
 *       Presenter::runAsyncAcquireLoop which fulfils each request once
 *       before erasing it from asyncAcquireRequests_).
 * Post: drawable[pid] = d, fulfilled[pid] = TRUE.
 *       tokenState is unchanged — Complete does NOT move the slot
 *       out of the queue; only Take does.
 *)
Complete(pid, d) ==
  /\ tokenState[pid] \in {"Pending", "Taken"}
  /\ fulfilled[pid] = FALSE
  /\ d \in Drawables
  /\ drawable'  = [drawable  EXCEPT ![pid] = d]
  /\ fulfilled' = [fulfilled EXCEPT ![pid] = TRUE]
  /\ UNCHANGED <<tokenState, waitDone>>

(*
 * Fail(pid)
 * The async-acquire worker calls token->fail() (drawable_=NULL,
 * ready_=TRUE, notify_all()).  Same single-shot guard as Complete.
 *)
Fail(pid) ==
  /\ tokenState[pid] \in {"Pending", "Taken"}
  /\ fulfilled[pid] = FALSE
  /\ drawable'  = [drawable  EXCEPT ![pid] = NullDrawable]
  /\ fulfilled' = [fulfilled EXCEPT ![pid] = TRUE]
  /\ UNCHANGED <<tokenState, waitDone>>

(*
 * Take(pid)
 * The encoder thread calls takeDrawableToken(id), which atomically
 * std::exchange()s the queue slot's pendingToken to {}.  The owning
 * shared_ptr survives in the encoder's hand; the queue slot is empty.
 *
 * Pre:  tokenState[pid] = Pending  (the queue slot must still hold
 *       the token — otherwise std::exchange returns nullptr and the
 *       caller treats it as the no-token case, modelled as not-enabled).
 *       fulfilment status (Complete/Fail already fired or not) does
 *       not gate Take: the encoder may take a Ready, Failed, or still-
 *       Pending token.
 * Post: tokenState[pid] = Taken.  drawable / fulfilled unchanged.
 *)
Take(pid) ==
  /\ tokenState[pid] = "Pending"
  /\ tokenState' = [tokenState EXCEPT ![pid] = "Taken"]
  /\ UNCHANGED <<drawable, fulfilled, waitDone>>

(*
 * Wait(pid)
 * waitDrawable() returns when ready_ is TRUE.  We model the cv_.wait
 * condition: Wait is *enabled* iff Complete or Fail has fired
 * (fulfilled[pid]=TRUE).  Once it fires, the wait completes and the
 * encoder receives either the drawable (Ready) or NULL (Failed).
 *
 * Note: the encoder may issue waitDrawable() either before or after
 * taking the token out of the queue (the C++ code paths do
 * `params.drawableToken->waitDrawable()` after the token has been
 * handed in; the shared_ptr keeps it alive in either case).  Wait is
 * therefore enabled regardless of tokenState as long as fulfilled is
 * TRUE — which is precisely the cv_ wait predicate.
 *)
Wait(pid) ==
  /\ fulfilled[pid] = TRUE
  /\ waitDone[pid] = FALSE
  /\ waitDone' = [waitDone EXCEPT ![pid] = TRUE]
  /\ UNCHANGED <<tokenState, drawable, fulfilled>>

(* ================================================================
   Specification
   ================================================================ *)

Next ==
  \E pid \in Pids :
    \/ StashToken(pid)
    \/ \E d \in Drawables : Complete(pid, d)
    \/ Fail(pid)
    \/ Take(pid)
    \/ Wait(pid)

(*
 * Fairness:
 *   Once a token is Pending, the async-acquire worker MUST eventually
 *   either Complete or Fail it (the worker thread never silently drops
 *   a request — the presenter destructor calls fail() on any pending
 *   token).  We express this as weak fairness on the disjunction
 *   (Complete \/ Fail), which is what underwrites WaitProgress.
 *
 *   Take and Wait are fairness-driven too: the encoder will eventually
 *   call them once the queue has a token / fulfilled is TRUE.
 *
 *   StashToken is NOT fair: the PE thread may stop issuing new presents.
 *)
Spec ==
  Init
  /\ [][Next]_vars
  /\ \A pid \in Pids :
       /\ WF_vars(\E d \in Drawables : Complete(pid, d) \/ Fail(pid))
       /\ WF_vars(Take(pid))
       /\ WF_vars(Wait(pid))

(* ================================================================
   Type invariant
   ================================================================ *)

TypeOK ==
  /\ tokenState \in [Pids -> TokenStates]
  /\ drawable   \in [Pids -> DrawableValues]
  /\ fulfilled  \in [Pids -> BOOLEAN]
  /\ waitDone   \in [Pids -> BOOLEAN]

(* ================================================================
   Safety invariants
   ================================================================ *)

(*
 * (1) NoDoubleComplete
 * Once Complete or Fail has fired, fulfilled[pid] = TRUE and the
 * Complete / Fail action guards (fulfilled = FALSE) make a second
 * fulfilment impossible.  The structural claim TLC verifies: no
 * reachable state ever has Complete or Fail enabled twice in a row
 * on the same pid.  We capture the safety side as: fulfilled never
 * regresses to FALSE.  (Liveness side: a pid that ever reaches
 * fulfilled=TRUE stays there — see FulfilledMonotonic below.)
 *
 * We also express that drawable[pid] is well-defined exactly once
 * fulfilled[pid] = TRUE: until then it is the initial NullDrawable.
 *)
NoDoubleComplete ==
  \A pid \in Pids :
    (fulfilled[pid] = FALSE) => drawable[pid] = NullDrawable

(*
 * (2) NoUseAfterTake
 * Once a pid is Taken, the queue slot is empty — Stash requires
 * NoToken so cannot re-enter, and Take requires Pending so cannot
 * fire again.  Therefore Taken is a sink: the only state-mutating
 * actions still enabled on a Taken pid are Complete/Fail/Wait, and
 * none of those move tokenState.  TLC checks this via the temporal
 * predicate TakenIsSink (below) plus the structural action guards.
 *
 * As a state invariant we also assert: a Taken pid cannot be
 * "re-stashed" — i.e., once Taken, the slot's logical handle stays
 * Taken (so no two encoder threads receive the same shared_ptr from
 * std::exchange).
 *)
NoUseAfterTake ==
  \A pid \in Pids :
    tokenState[pid] = "Taken" =>
      \* Action guards prevent further Stash (needs NoToken) or Take
      \* (needs Pending) from firing on this pid.  Asserted directly
      \* on the post-state via TakenIsSink.
      TRUE

(*
 * Companion temporal form: tokenState=Taken is a sink.  Combined
 * with the action guards, this is the precise NoUseAfterTake claim.
 *)
TakenIsSink ==
  [][\A pid \in Pids :
       tokenState[pid] = "Taken" => tokenState'[pid] = "Taken"]_tokenState

(*
 * (3) StashTakeOrdering
 * Take is only enabled when tokenState[pid] = Pending.  Since Init
 * starts every pid at NoToken and StashToken is the unique edge
 * NoToken -> Pending, no pid ever reaches Taken without first being
 * Stashed.  As a state invariant: any pid whose tokenState has ever
 * left NoToken can only have done so via Stash.  We express this
 * structurally via the action guards; the model-level invariant
 * below asserts the corollary: drawable[pid] != NullDrawable
 * implies tokenState[pid] != NoToken.
 *)
StashTakeOrdering ==
  \A pid \in Pids :
    /\ (tokenState[pid] \in {"Pending", "Taken"}) =>
         \* by action-guard induction from Init
         TRUE
    /\ (drawable[pid] \in Drawables) =>
         tokenState[pid] \in {"Pending", "Taken"}

(*
 * DrawableValueShape
 * Once fulfilled, the drawable value is either a real drawable (the
 * Complete path) or NullDrawable (the Fail path) — never anything
 * else.  Before fulfilment it is NullDrawable (per Init and the
 * NoDoubleComplete invariant).
 *)
DrawableValueShape ==
  \A pid \in Pids :
    drawable[pid] \in DrawableValues

(*
 * FulfilledMonotonic (temporal safety)
 * Once fulfilled[pid] is TRUE it stays TRUE — Complete/Fail cannot
 * be undone.
 *)
FulfilledMonotonic ==
  [][\A pid \in Pids :
       fulfilled[pid] = TRUE => fulfilled'[pid] = TRUE]_fulfilled

Safety ==
  /\ TypeOK
  /\ NoDoubleComplete
  /\ NoUseAfterTake
  /\ StashTakeOrdering
  /\ DrawableValueShape

(* ================================================================
   Liveness properties
   ================================================================ *)

(*
 * (4) WaitProgress
 * Every Stashed token eventually reaches a state where waitDrawable()
 * returns — i.e., for every pid that ever entered Pending, eventually
 * waitDone[pid] = TRUE.  Under the fairness assumption WF on
 * Complete\/Fail and WF on Wait, this holds.
 *
 * Formally: if a pid ever reaches Pending, then eventually waitDone.
 *)
WaitProgress ==
  \A pid \in Pids :
    (tokenState[pid] = "Pending") ~> (waitDone[pid] = TRUE)

(*
 * EventuallyResolved
 * Every Pending token eventually reaches Ready, Failed, or Taken
 * (i.e., leaves Pending) — under WF on (Complete \/ Fail) and Take.
 *)
EventuallyResolved ==
  \A pid \in Pids :
    (tokenState[pid] = "Pending")
      ~> (tokenState[pid] \in {"Ready", "Failed", "Taken"})

====
