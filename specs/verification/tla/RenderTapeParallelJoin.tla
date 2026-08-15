---- MODULE RenderTapeParallelJoin ----
(***************************************************************************
 * Bounded Render Tape refinement for the successful ExplicitParallel lane.
 *
 * The parent owns one Clear, a FULL_SNAPSHOT DrawRun anchor, and one Present.
 * A fixed two-child partition
 * owns a non-empty Draw range; workers may encode their ranges in any
 * interleaving, but the coordinator joins children in partition order before
 * publishing the Present.  identityOutput is the serial/identity reference
 * stream.  parallelOutput is the stream visible after the joined children.
 * Exact equality at Present is the refinement claim; this model says nothing
 * about Metal pixels or performance.
 *
 * The fixed bounds are intentional: this is the smallest model containing a
 * distinguishing child interleaving.  Draws 1..2 belong to child 1 and the
 * remaining draws belong to child 2.  The supplied config uses four draws,
 * hence both children have work and the model cannot pass vacuously.
 ****************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS ChildCount, DrawCount

VARIABLES phase, selectedCount, snapshotAnchored, childState, encoded,
          childOutput, identityNext, identityOutput, parallelOutput,
          workerWork, joinedCount, presented

Children == 1 .. ChildCount
Draws == 1 .. DrawCount

ChildOf(draw) == IF draw <= 2 THEN 1 ELSE 2
OwnedDraws(child) == {draw \in Draws : ChildOf(draw) = child}

ChildStates == {"Absent", "Created", "Encoding", "Ended", "Joined"}
Phases == {"BeforeClear", "Cleared", "ChildrenCreated", "Joined", "Presented"}

RECURSIVE SerialPrefix(_)
SerialPrefix(n) == IF n = 0 THEN <<>> ELSE Append(SerialPrefix(n - 1), n)

vars ==
  << phase, selectedCount, snapshotAnchored, childState, encoded, childOutput,
     identityNext, identityOutput, parallelOutput, workerWork,
     joinedCount, presented >>

Init ==
  /\ phase = "BeforeClear"
  /\ selectedCount = 0
  /\ snapshotAnchored = FALSE
  /\ childState = [child \in Children |-> "Absent"]
  /\ encoded = [draw \in Draws |-> FALSE]
  /\ childOutput = [child \in Children |-> <<>>]
  /\ identityNext = 0
  /\ identityOutput = <<>>
  /\ parallelOutput = <<>>
  /\ workerWork = 0
  /\ joinedCount = 0
  /\ presented = FALSE

ClearStep ==
  /\ phase = "BeforeClear"
  /\ selectedCount = 0
  /\ phase' = "Cleared"
  /\ selectedCount' = 1
  /\ identityOutput' = Append(identityOutput, 0)
  /\ parallelOutput' = Append(parallelOutput, 0)
  /\ UNCHANGED << snapshotAnchored, childState, encoded, childOutput,
                  identityNext, workerWork, joinedCount, presented >>

FullSnapshotDrawRunStep ==
  /\ phase = "Cleared"
  /\ selectedCount = 1
  /\ ~snapshotAnchored
  /\ snapshotAnchored' = TRUE
  /\ UNCHANGED << phase, selectedCount, childState, encoded, childOutput,
                  identityNext, identityOutput, parallelOutput, workerWork,
                  joinedCount, presented >>

CreateChild(child) ==
  /\ phase = "Cleared"
  /\ snapshotAnchored
  /\ child \in Children
  /\ childState[child] = "Absent"
  /\ \A prior \in Children : prior < child => childState[prior] # "Absent"
  /\ childState' = [childState EXCEPT ![child] = "Created"]
  /\ phase' = IF child = ChildCount THEN "ChildrenCreated" ELSE phase
  /\ UNCHANGED << selectedCount, snapshotAnchored, encoded, childOutput, identityNext,
                  identityOutput, parallelOutput, workerWork, joinedCount,
                  presented >>

BeginChild(child) ==
  /\ phase = "ChildrenCreated"
  /\ \A candidate \in Children : childState[candidate] # "Absent"
  /\ child \in Children
  /\ childState[child] = "Created"
  /\ childState' = [childState EXCEPT ![child] = "Encoding"]
  /\ UNCHANGED << phase, selectedCount, snapshotAnchored, encoded, childOutput, identityNext,
                  identityOutput, parallelOutput, workerWork, joinedCount,
                  presented >>

IdentityDraw ==
  /\ phase # "BeforeClear"
  /\ selectedCount = 1
  /\ snapshotAnchored
  /\ identityNext < DrawCount
  /\ identityNext' = identityNext + 1
  /\ identityOutput' = Append(identityOutput, identityNext + 1)
  /\ UNCHANGED << phase, selectedCount, snapshotAnchored, childState, encoded, childOutput,
                  parallelOutput, workerWork, joinedCount, presented >>

NextOwnedDraw(child) ==
  CHOOSE draw \in OwnedDraws(child) :
    /\ ~encoded[draw]
    /\ \A prior \in OwnedDraws(child) : prior < draw => encoded[prior]

EncodeDraw(child) ==
  /\ phase = "ChildrenCreated"
  /\ child \in Children
  /\ childState[child] = "Encoding"
  /\ \E draw \in OwnedDraws(child) : ~encoded[draw]
  /\ LET draw == NextOwnedDraw(child) IN
       /\ encoded' = [encoded EXCEPT ![draw] = TRUE]
       /\ childOutput' =
            [childOutput EXCEPT ![child] = Append(@, draw)]
  /\ workerWork' = workerWork + 1
  /\ UNCHANGED << phase, selectedCount, snapshotAnchored, childState, identityNext,
                  identityOutput, parallelOutput, joinedCount, presented >>

EndChild(child) ==
  /\ phase = "ChildrenCreated"
  /\ child \in Children
  /\ childState[child] = "Encoding"
  /\ \A draw \in OwnedDraws(child) : encoded[draw]
  /\ childState' = [childState EXCEPT ![child] = "Ended"]
  /\ UNCHANGED << phase, selectedCount, snapshotAnchored, encoded, childOutput, identityNext,
                  identityOutput, parallelOutput, workerWork, joinedCount,
                  presented >>

JoinChild(child) ==
  /\ phase = "ChildrenCreated"
  /\ child \in Children
  /\ childState[child] = "Ended"
  /\ \A prior \in Children : prior < child => childState[prior] = "Joined"
  /\ childState' = [childState EXCEPT ![child] = "Joined"]
  /\ joinedCount' = joinedCount + 1
  /\ parallelOutput' = parallelOutput \o childOutput[child]
  /\ phase' = IF child = ChildCount THEN "Joined" ELSE phase
  /\ UNCHANGED << selectedCount, snapshotAnchored, encoded, childOutput, identityNext,
                  identityOutput, workerWork, presented >>

PresentStep ==
  /\ phase = "Joined"
  /\ ~presented
  /\ snapshotAnchored
  /\ identityNext = DrawCount
  /\ \A draw \in Draws : encoded[draw]
  /\ \A child \in Children : childState[child] = "Joined"
  /\ parallelOutput = identityOutput
  /\ phase' = "Presented"
  /\ presented' = TRUE
  /\ UNCHANGED << selectedCount, snapshotAnchored, childState, encoded, childOutput,
                  identityNext, identityOutput, parallelOutput, workerWork,
                  joinedCount >>

CoordinatorStep ==
  \/ ClearStep
  \/ FullSnapshotDrawRunStep
  \/ \E child \in Children : CreateChild(child)
  \/ \E child \in Children : BeginChild(child)
  \/ \E child \in Children : EndChild(child)
  \/ \E child \in Children : JoinChild(child)
  \/ PresentStep

WorkerStep ==
  \/ IdentityDraw
  \/ \E child \in Children : EncodeDraw(child)

Next == CoordinatorStep \/ WorkerStep

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(CoordinatorStep)
  /\ WF_vars(WorkerStep)

TypeOK ==
  /\ ChildCount = 2
  /\ DrawCount >= 4
  /\ phase \in Phases
  /\ selectedCount \in Nat
  /\ snapshotAnchored \in BOOLEAN
  /\ childState \in [Children -> ChildStates]
  /\ encoded \in [Draws -> BOOLEAN]
  /\ childOutput \in [Children -> Seq(Nat)]
  /\ identityNext \in 0 .. DrawCount
  /\ identityOutput \in Seq(Nat)
  /\ parallelOutput \in Seq(Nat)
  /\ workerWork \in Nat
  /\ joinedCount \in 0 .. ChildCount
  /\ presented \in BOOLEAN

SelectionAndPartitionNonVacuous ==
  /\ presented => selectedCount > 0
  /\ presented => ChildCount >= 2
  /\ presented => DrawCount > 0
  /\ presented => workerWork > 0
  /\ presented => snapshotAnchored
  /\ presented => Cardinality({child \in Children :
                                childState[child] = "Joined"}) >= 2

IdentitySerialOrder ==
  /\ selectedCount = 0 => identityOutput = <<>>
  /\ selectedCount > 0 =>
       identityOutput = <<0>> \o SerialPrefix(identityNext)

ChildWorkIsOrderedAndOwned ==
  /\ \A child \in Children :
       /\ childOutput[child] \in Seq(OwnedDraws(child))
       /\ \A i \in 1 .. (Len(childOutput[child]) - 1) :
            childOutput[child][i] < childOutput[child][i + 1]
       /\ Len(childOutput[child]) =
            Cardinality({draw \in OwnedDraws(child) : encoded[draw]})
  /\ workerWork = Cardinality({draw \in Draws : encoded[draw]})

ChildrenJoinInOrder ==
  /\ \A child \in Children :
       childState[child] = "Joined" <=> child <= joinedCount

ParallelOutputPreservesSerialOrder ==
  /\ selectedCount = 0 => parallelOutput = <<>>
  /\ selectedCount > 0 =>
       parallelOutput = <<0>> \o SerialPrefix(Len(parallelOutput) - 1)

ClearAndPresentAreCoordinatorOwned ==
  /\ presented => parallelOutput[1] = 0
  /\ presented => phase = "Presented"
  /\ presented => snapshotAnchored
  /\ presented => \A child \in Children : childState[child] = "Joined"

ExactIdentityRefinement ==
  presented =>
    /\ parallelOutput = identityOutput
    /\ parallelOutput = <<0>> \o SerialPrefix(DrawCount)
    /\ Len(parallelOutput) = DrawCount + 1

SnapshotPrecedesDraw ==
  /\ identityNext > 0 => snapshotAnchored
  /\ \A draw \in Draws : encoded[draw] => snapshotAnchored

Inv ==
  /\ TypeOK
  /\ SelectionAndPartitionNonVacuous
  /\ IdentitySerialOrder
  /\ ChildWorkIsOrderedAndOwned
  /\ ChildrenJoinInOrder
  /\ ParallelOutputPreservesSerialOrder
  /\ ClearAndPresentAreCoordinatorOwned
  /\ ExactIdentityRefinement
  /\ SnapshotPrecedesDraw

ClearProgress ==
  phase = "BeforeClear" ~> selectedCount > 0

IdentityProgress ==
  selectedCount > 0 ~> identityNext = DrawCount

SnapshotProgress ==
  selectedCount > 0 ~> snapshotAnchored

WorkerWorkProgress ==
  selectedCount > 0 ~> workerWork > 0

JoinProgress ==
  selectedCount > 0 ~>
    (\A child \in Children : childState[child] = "Joined")

PresentProgress ==
  selectedCount > 0 ~> presented

====
