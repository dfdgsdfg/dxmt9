---- MODULE ParallelDrawBinding ----
(***************************************************************************
 * R-BACK-2.63 / R-VERIF-2.15 source-local parallel draw-binding refinement.
 *
 * Child 1 encodes A -> B -> A and child 2 supplies an independent sibling
 * shadow. PrepareDraw is the model counterpart of
 * planDrawBindingTransition(): a generation change first marks the child
 * dirty, BindCurrent installs the required generation, and only a clean
 * child may encode. If PrepareDraw is changed to omit that dirty edge,
 * EncodeCurrent remains enabled with the stale bound generation and TLC
 * reports DrawUsesRequiredUniformGeneration immediately.
 *
 * A slot-30 argument-table PSO or a mixed Stage 1/Stage 2b pass cannot enter
 * direct-binding children. The coordinator validates one pass-wide ABI before
 * any Metal side effect and abstracts an incompatible plan as a complete
 * serial fallback.
 ****************************************************************************)

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS ChildCount, DrawCount, GenerationCount

VARIABLES childState, currentDraw, requiredGeneration, boundGeneration,
          dirty, psoAbi, encoded, boundAtDraw, parentEnded, completion,
          serialFallback, terminalFailure

Children == 1 .. ChildCount
Draws == 1 .. DrawCount
Generations == 1 .. GenerationCount
Generation0 == 0 .. GenerationCount

ChildStates == {"Absent", "Created", "Encoding", "Ended", "Failed", "Joined"}
PsoAbis == {"Stage1Direct", "Stage2DirectCbuf", "Stage2ArgumentTable"}

Owner(draw) == IF draw <= 3 THEN 1 ELSE 2
RequiredGeneration(draw) == IF draw = 2 \/ draw = 4 THEN 2 ELSE 1
AbiCompatible(abi) == abi \in {"Stage1Direct", "Stage2DirectCbuf"}

OwnedDraws(child) == {draw \in Draws : Owner(draw) = child}
AllChildrenCreated == \A child \in Children : childState[child] # "Absent"
AllChildAbisCompatible ==
  /\ \A child \in Children : AbiCompatible(psoAbi[child])
  /\ \A child \in Children : psoAbi[child] = psoAbi[1]
AllOwnedEncoded(child) ==
  \A draw \in OwnedDraws(child) : encoded[draw]
AllDrawsEncoded == \A draw \in Draws : encoded[draw]
NoDrawEncoded == \A draw \in Draws : ~encoded[draw]

LowerOwnedEncoded(child, draw) ==
  \A prior \in OwnedDraws(child) : prior < draw => encoded[prior]

HasEncodedDraw(child) == \E draw \in OwnedDraws(child) : encoded[draw]
LastEncodedDraw(child) ==
  CHOOSE draw \in OwnedDraws(child) :
    /\ encoded[draw]
    /\ \A later \in OwnedDraws(child) : later > draw => ~encoded[later]

vars == <<childState, currentDraw, requiredGeneration, boundGeneration,
          dirty, psoAbi, encoded, boundAtDraw, parentEnded, completion,
          serialFallback, terminalFailure>>

Init ==
  /\ childState = [child \in Children |-> "Absent"]
  /\ currentDraw = [child \in Children |-> 0]
  /\ requiredGeneration = [child \in Children |-> 0]
  /\ boundGeneration = [child \in Children |-> 0]
  /\ dirty = [child \in Children |-> FALSE]
  /\ psoAbi = [child \in Children |-> "Stage1Direct"]
  /\ encoded = [draw \in Draws |-> FALSE]
  /\ boundAtDraw = [draw \in Draws |-> 0]
  /\ parentEnded = FALSE
  /\ completion = FALSE
  /\ serialFallback = FALSE
  /\ terminalFailure = FALSE

CreateChild(child, abi) ==
  /\ child \in Children
  /\ abi \in PsoAbis
  /\ childState[child] = "Absent"
  /\ \A prior \in Children : prior < child => childState[prior] # "Absent"
  /\ childState' = [childState EXCEPT ![child] = "Created"]
  /\ psoAbi' = [psoAbi EXCEPT ![child] = abi]
  /\ dirty' = [dirty EXCEPT ![child] = TRUE]
  /\ UNCHANGED <<currentDraw, requiredGeneration, boundGeneration,
                  encoded, boundAtDraw, parentEnded, completion,
                  serialFallback, terminalFailure>>

BeginChild(child) ==
  /\ child \in Children
  /\ AllChildrenCreated
  /\ AllChildAbisCompatible
  /\ childState[child] = "Created"
  /\ ~terminalFailure
  /\ childState' = [childState EXCEPT ![child] = "Encoding"]
  /\ UNCHANGED <<currentDraw, requiredGeneration, boundGeneration, dirty,
                  psoAbi, encoded, boundAtDraw, parentEnded, completion,
                  serialFallback, terminalFailure>>

FallbackBeforeMetalSideEffect ==
  /\ AllChildrenCreated
  /\ ~AllChildAbisCompatible
  /\ NoDrawEncoded
  /\ childState' = [child \in Children |-> "Joined"]
  /\ encoded' = [draw \in Draws |-> TRUE]
  /\ boundAtDraw' =
       [draw \in Draws |-> RequiredGeneration(draw)]
  /\ boundGeneration' = [child \in Children |-> 0]
  /\ currentDraw' = [child \in Children |-> 0]
  /\ requiredGeneration' = [child \in Children |-> 0]
  /\ dirty' = [child \in Children |-> FALSE]
  /\ serialFallback' = TRUE
  /\ UNCHANGED <<psoAbi, parentEnded, completion, terminalFailure>>

PrepareDraw(child, draw) ==
  /\ child \in Children
  /\ draw \in OwnedDraws(child)
  /\ childState[child] = "Encoding"
  /\ ~terminalFailure
  /\ currentDraw[child] = 0
  /\ ~encoded[draw]
  /\ LowerOwnedEncoded(child, draw)
  /\ currentDraw' = [currentDraw EXCEPT ![child] = draw]
  /\ requiredGeneration' =
       [requiredGeneration EXCEPT ![child] = RequiredGeneration(draw)]
  /\ dirty' = [dirty EXCEPT
       ![child] = @ \/ boundGeneration[child] # RequiredGeneration(draw)]
  /\ UNCHANGED <<childState, boundGeneration, psoAbi, encoded, boundAtDraw,
                  parentEnded, completion, serialFallback, terminalFailure>>

BindCurrent(child) ==
  /\ child \in Children
  /\ childState[child] = "Encoding"
  /\ ~terminalFailure
  /\ currentDraw[child] # 0
  /\ dirty[child]
  /\ boundGeneration' =
       [boundGeneration EXCEPT ![child] = requiredGeneration[child]]
  /\ dirty' = [dirty EXCEPT ![child] = FALSE]
  /\ UNCHANGED <<childState, currentDraw, requiredGeneration, psoAbi,
                  encoded, boundAtDraw, parentEnded, completion,
                  serialFallback, terminalFailure>>

EncodeCurrent(child) ==
  /\ child \in Children
  /\ childState[child] = "Encoding"
  /\ ~terminalFailure
  /\ currentDraw[child] # 0
  /\ ~dirty[child]
  /\ AbiCompatible(psoAbi[child])
  /\ encoded' = [encoded EXCEPT ![currentDraw[child]] = TRUE]
  /\ boundAtDraw' =
       [boundAtDraw EXCEPT ![currentDraw[child]] = boundGeneration[child]]
  /\ currentDraw' = [currentDraw EXCEPT ![child] = 0]
  /\ requiredGeneration' = [requiredGeneration EXCEPT ![child] = 0]
  /\ UNCHANGED <<childState, boundGeneration, dirty, psoAbi, parentEnded,
                  completion, serialFallback, terminalFailure>>

EndChild(child) ==
  /\ child \in Children
  /\ childState[child] = "Encoding"
  /\ ~terminalFailure
  /\ currentDraw[child] = 0
  /\ AllOwnedEncoded(child)
  /\ childState' = [childState EXCEPT ![child] = "Ended"]
  /\ UNCHANGED <<currentDraw, requiredGeneration, boundGeneration, dirty,
                  psoAbi, encoded, boundAtDraw, parentEnded, completion,
                  serialFallback, terminalFailure>>

WorkerFailure ==
  /\ ~terminalFailure
  /\ AllChildrenCreated
  /\ \E child \in Children :
       childState[child] \in {"Created", "Encoding"}
  /\ childState' = [child \in Children |->
       IF childState[child] \in {"Created", "Encoding"}
       THEN "Failed" ELSE childState[child]]
  /\ currentDraw' = [child \in Children |-> 0]
  /\ requiredGeneration' = [child \in Children |-> 0]
  /\ boundGeneration' = [child \in Children |-> 0]
  /\ dirty' = [child \in Children |-> FALSE]
  /\ terminalFailure' = TRUE
  /\ UNCHANGED <<psoAbi, encoded, boundAtDraw, parentEnded, completion,
                  serialFallback>>

JoinChild(child) ==
  /\ child \in Children
  /\ childState[child] \in {"Ended", "Failed"}
  /\ \A prior \in Children : prior < child => childState[prior] = "Joined"
  /\ childState' = [childState EXCEPT ![child] = "Joined"]
  /\ UNCHANGED <<currentDraw, requiredGeneration, boundGeneration, dirty,
                  psoAbi, encoded, boundAtDraw, parentEnded, completion,
                  serialFallback, terminalFailure>>

EndParent ==
  /\ ~parentEnded
  /\ \A child \in Children : childState[child] = "Joined"
  /\ parentEnded' = TRUE
  /\ UNCHANGED <<childState, currentDraw, requiredGeneration,
                  boundGeneration, dirty, psoAbi, encoded, boundAtDraw,
                  completion, serialFallback, terminalFailure>>

Complete ==
  /\ parentEnded
  /\ ~completion
  /\ completion' = TRUE
  /\ UNCHANGED <<childState, currentDraw, requiredGeneration,
                  boundGeneration, dirty, psoAbi, encoded, boundAtDraw,
                  parentEnded, serialFallback, terminalFailure>>

CoordinatorStep ==
  \/ \E child \in Children, abi \in PsoAbis : CreateChild(child, abi)
  \/ FallbackBeforeMetalSideEffect
  \/ WorkerFailure
  \/ \E child \in Children : JoinChild(child)
  \/ EndParent
  \/ Complete

WorkerStep ==
  \/ \E child \in Children : BeginChild(child)
  \/ \E child \in Children, draw \in Draws : PrepareDraw(child, draw)
  \/ \E child \in Children : BindCurrent(child)
  \/ \E child \in Children : EncodeCurrent(child)
  \/ \E child \in Children : EndChild(child)

Next == CoordinatorStep \/ WorkerStep

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(CoordinatorStep)
  /\ WF_vars(WorkerStep)

TypeOK ==
  /\ childState \in [Children -> ChildStates]
  /\ currentDraw \in [Children -> 0 .. DrawCount]
  /\ requiredGeneration \in [Children -> Generation0]
  /\ boundGeneration \in [Children -> Generation0]
  /\ dirty \in [Children -> BOOLEAN]
  /\ psoAbi \in [Children -> PsoAbis]
  /\ encoded \in [Draws -> BOOLEAN]
  /\ boundAtDraw \in [Draws -> Generation0]
  /\ parentEnded \in BOOLEAN
  /\ completion \in BOOLEAN
  /\ serialFallback \in BOOLEAN
  /\ terminalFailure \in BOOLEAN

DrawUsesRequiredUniformGeneration ==
  \A draw \in Draws :
    encoded[draw] => boundAtDraw[draw] = RequiredGeneration(draw)

PsoBindingAbiMatchesChildBinding ==
  \A child \in Children :
    childState[child] \in {"Encoding", "Ended", "Joined"} /\
    ~serialFallback /\ ~terminalFailure => AbiCompatible(psoAbi[child])

ChildBindingShadowsAreIsolated ==
  \A child \in Children :
    /\ boundGeneration[child] \in Generation0
    /\ \/ boundGeneration[child] = 0
       \/ /\ currentDraw[child] # 0
          /\ boundGeneration[child] = requiredGeneration[child]
       \/ \E draw \in OwnedDraws(child) :
            encoded[draw] /\
            boundGeneration[child] = RequiredGeneration(draw)

DrawsExecuteExactlyOnceInSerialOrder ==
  /\ \A draw \in Draws :
       encoded[draw] => LowerOwnedEncoded(Owner(draw), draw)
  /\ \A left, right \in Draws :
       left < right => Owner(left) <= Owner(right)
  /\ completion => AllDrawsEncoded \/ terminalFailure

AllChildrenEndBeforeParent ==
  parentEnded => \A child \in Children : childState[child] = "Joined"

CompletionAfterJoinedParent ==
  completion => parentEnded /\
      \A child \in Children : childState[child] = "Joined"

Inv ==
  /\ TypeOK
  /\ DrawUsesRequiredUniformGeneration
  /\ PsoBindingAbiMatchesChildBinding
  /\ ChildBindingShadowsAreIsolated
  /\ DrawsExecuteExactlyOnceInSerialOrder
  /\ AllChildrenEndBeforeParent
  /\ CompletionAfterJoinedParent

CreatedChildEventuallyJoinedOrFallback ==
  \A child \in Children :
    childState[child] = "Created" ~>
      childState[child] = "Joined" \/ serialFallback \/ terminalFailure

ParentAndCompletionProgress ==
  AllChildrenCreated ~> completion

====
