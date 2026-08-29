---- MODULE StateBlockOrderedReplay ----
(***************************************************************************
 * The smallest refinement for the fourth ReplayOffloadQueue alternative:
 * a state-block apply is one FIFO item, not a producer-side global drain.
 * The native binding is managed_mutation_offload_transaction_spec's
 * Chunk -> StateBlock -> Chunk worker trace.
 *************************************************************************** *)

EXTENDS Naturals, Sequences, TLC

Items == <<"ChunkA", "StateBlock", "ChunkB">>
ItemSet == {"ChunkA", "StateBlock", "ChunkB"}
NoItem == "None"
VARIABLES running, poisoned, accepted, queue, inFlight, completed,
          stateBlockApplied, parentJoined

vars == <<running, poisoned, accepted, queue, inFlight, completed,
           stateBlockApplied, parentJoined>>

CompletedContains(item) ==
  \E i \in 1..Len(completed): completed[i] = item

Init ==
  /\ running = TRUE
  /\ poisoned = FALSE
  /\ accepted = <<>>
  /\ queue = <<>>
  /\ inFlight = NoItem
  /\ completed = <<>>
  /\ stateBlockApplied = FALSE
  /\ parentJoined = FALSE

EnqueueNext ==
  /\ running
  /\ ~poisoned
  /\ Len(accepted) < Len(Items)
  /\ accepted' = Append(accepted, Items[Len(accepted) + 1])
  /\ queue' = Append(queue, Items[Len(accepted) + 1])
  /\ UNCHANGED <<running, poisoned, inFlight, completed, stateBlockApplied,
                 parentJoined>>

StartReplay ==
  /\ running
  /\ ~poisoned
  /\ inFlight = NoItem
  /\ Len(queue) > 0
  /\ inFlight' = Head(queue)
  /\ queue' = Tail(queue)
  /\ UNCHANGED <<running, poisoned, accepted, completed, stateBlockApplied,
                 parentJoined>>

(* Deliberately bypass the FIFO head. This is not production behavior; the
 * companion configuration must continue to find the short counterexample. *)
BadStartReplay ==
  /\ running
  /\ ~poisoned
  /\ inFlight = NoItem
  /\ Len(queue) = Len(Items)
  /\ inFlight' = queue[Len(queue)]
  /\ queue' = SubSeq(queue, 1, Len(queue) - 1)
  /\ UNCHANGED <<running, poisoned, accepted, completed, stateBlockApplied,
                 parentJoined>>

FinishReplay ==
  /\ running
  /\ ~poisoned
  /\ inFlight \in ItemSet
  /\ completed' = Append(completed, inFlight)
  /\ stateBlockApplied' =
       IF inFlight = "StateBlock" THEN TRUE ELSE stateBlockApplied
  /\ inFlight' = NoItem
  /\ UNCHANGED <<running, poisoned, accepted, queue, parentJoined>>

JoinParent ==
  /\ running
  /\ ~poisoned
  /\ ~parentJoined
  /\ completed = Items
  /\ parentJoined' = TRUE
  /\ UNCHANGED <<running, poisoned, accepted, queue, inFlight, completed,
                 stateBlockApplied>>

FailReplay ==
  /\ running
  /\ ~poisoned
  /\ inFlight \in ItemSet
  /\ poisoned' = TRUE
  /\ running' = FALSE
  /\ queue' = <<>>
  /\ UNCHANGED <<accepted, inFlight, completed, stateBlockApplied,
                 parentJoined>>

Next == EnqueueNext \/ StartReplay \/ FinishReplay \/ JoinParent \/ FailReplay

TypeOK ==
  /\ running \in BOOLEAN
  /\ poisoned \in BOOLEAN
  /\ accepted \in Seq(ItemSet)
  /\ queue \in Seq(ItemSet)
  /\ inFlight \in ItemSet \cup {NoItem}
  /\ completed \in Seq(ItemSet)
  /\ stateBlockApplied \in BOOLEAN
  /\ parentJoined \in BOOLEAN

(* A worker cannot observe a later item before the FIFO prefix preceding it. *)
CompletedIsAcceptedPrefix ==
  completed = SubSeq(accepted, 1, Len(completed))

(* StateBlock's effect is visible only after the worker finishes that item. *)
StateBlockApplyIsCompleted ==
  stateBlockApplied => CompletedContains("StateBlock")

(* The second chunk cannot complete before its ordered StateBlock item. *)
ChunkBRequiresStateBlock ==
  CompletedContains("ChunkB") => CompletedContains("StateBlock")

(* The parent completion boundary is after every child/FIFO item. *)
ParentCompletionAfterChildren ==
  parentJoined => completed = Items

ParentEventuallyJoined ==
  <>parentJoined \/ <>poisoned

Spec ==
  Init /\ [][Next]_vars
    /\ WF_vars(EnqueueNext)
    /\ WF_vars(StartReplay)
    /\ WF_vars(FinishReplay)
    /\ WF_vars(JoinParent)

BadSpec ==
  Init /\ [][Next \/ BadStartReplay]_vars

====
