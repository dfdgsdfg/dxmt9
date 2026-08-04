---- MODULE ReplayScopedDrain ----
(***************************************************************************
 * R-BACK-2.51(d): resource-scoped commit-replay drains.                   *
 *                                                                         *
 * Queue admission, worker ownership, replay completion, and failure       *
 * publication are separate transitions. Raw backing residency covers both *
 * queued and popped-but-in-flight chunks.                                 *
****************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

Resources == {"A", "B"}
BufferAliases == {"A0", "A1", "B0"}
Chunks == {1, 2, 3}
Generations == {"G0", "G1"}
ChunkStates == {"New", "Queued", "InFlight", "Done", "Failed",
                 "Abandoned", "Refused"}
WaitStates == {"Idle", "Waiting", "CaughtUp", "Stopped", "Poisoned"}
NoChunk == 0
NoGeneration == "None"

Target(c) == IF c = 3 THEN "B" ELSE "A"
CoreIdentity(w) == IF w \in {"A0", "A1"} THEN "A" ELSE "B"
Representative(r) == IF r = "A" THEN "A0" ELSE "B0"
SeqToSet(s) == {s[i] : i \in 1..Len(s)}

VARIABLES
  running,
  poisoned,
  failureObserved,
  failureCutoff,
  nextSeq,
  queue,
  inFlightChunk,
  chunkState,
  replaySeq,
  lastQueued,
  lastReplayed,
  currentGeneration,
  capturedGeneration,
  emittedGeneration,
  rawResidency,
  waitState,
  globalWaitState,
  rejectedSeqs,
  trace

vars == <<running, poisoned, failureObserved, failureCutoff, nextSeq, queue,
          inFlightChunk, chunkState, replaySeq, lastQueued, lastReplayed,
          currentGeneration, capturedGeneration, emittedGeneration,
          rawResidency, waitState, globalWaitState, rejectedSeqs, trace>>

ZeroByResource == [r \in Resources |-> 0]
EmptyGenerationByChunk == [c \in Chunks |-> NoGeneration]
EmptyRawResidency ==
  [r \in Resources |-> [g \in Generations |-> {}]]

Init ==
  /\ running = TRUE
  /\ poisoned = FALSE
  /\ failureObserved = FALSE
  /\ failureCutoff = 0
  /\ nextSeq = 1
  /\ queue = <<>>
  /\ inFlightChunk = NoChunk
  /\ chunkState = [c \in Chunks |-> "New"]
  /\ replaySeq = [c \in Chunks |-> 0]
  /\ lastQueued = ZeroByResource
  /\ lastReplayed = ZeroByResource
  /\ currentGeneration = [r \in Resources |-> "G0"]
  /\ capturedGeneration = EmptyGenerationByChunk
  /\ emittedGeneration = EmptyGenerationByChunk
  /\ rawResidency = EmptyRawResidency
  /\ waitState = [r \in Resources |-> "Idle"]
  /\ globalWaitState = "Idle"
  /\ rejectedSeqs = {}
  /\ trace = <<>>

(* Queue ownership and ledger publication are one accepted transition. *)
AcceptPush(c) ==
  /\ running
  /\ ~poisoned
  /\ ~failureObserved
  /\ chunkState[c] = "New"
  /\ queue' = Append(queue, c)
  /\ chunkState' = [chunkState EXCEPT ![c] = "Queued"]
  /\ replaySeq' = [replaySeq EXCEPT ![c] = nextSeq]
  /\ lastQueued' = [lastQueued EXCEPT ![Target(c)] = nextSeq]
  /\ capturedGeneration' =
       [capturedGeneration EXCEPT ![c] = currentGeneration[Target(c)]]
  /\ rawResidency' =
       [rawResidency EXCEPT
          ![Target(c)][currentGeneration[Target(c)]] = @ \cup {c}]
  /\ globalWaitState' =
       IF globalWaitState = "CaughtUp" THEN "Idle" ELSE globalWaitState
  /\ waitState' =
       [waitState EXCEPT ![Target(c)] =
         IF @ = "CaughtUp" THEN "Idle" ELSE @]
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff,
                  inFlightChunk, lastReplayed, currentGeneration,
                  emittedGeneration, rejectedSeqs, trace>>

(***************************************************************************
 * A stopped or failure-published push is refused without a sequence or     *
 * watermark. This includes the adversarial publish-then-refused shape.     *
****************************************************************************)
PublishThenRefusedPush(c) ==
  /\ (~running \/ poisoned \/ failureObserved)
  /\ chunkState[c] = "New"
  /\ chunkState' = [chunkState EXCEPT ![c] = "Refused"]
  /\ rejectedSeqs' = rejectedSeqs \cup {nextSeq}
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, replaySeq, lastQueued, lastReplayed,
                  currentGeneration, capturedGeneration, emittedGeneration,
                  rawResidency, waitState, globalWaitState, trace>>

(* Pop transfers ownership to the worker without releasing residency. *)
StartReplay ==
  /\ running
  /\ ~poisoned
  /\ ~failureObserved
  /\ inFlightChunk = NoChunk
  /\ Len(queue) > 0
  /\ LET c == Head(queue) IN
       /\ queue' = Tail(queue)
       /\ inFlightChunk' = c
       /\ chunkState' = [chunkState EXCEPT ![c] = "InFlight"]
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  replaySeq, lastQueued, lastReplayed, currentGeneration,
                  capturedGeneration, emittedGeneration, rawResidency,
                  waitState, globalWaitState, rejectedSeqs, trace>>

FinishReplaySuccess ==
  /\ running
  /\ ~poisoned
  /\ ~failureObserved
  /\ inFlightChunk \in Chunks
  /\ LET c == inFlightChunk IN
       /\ chunkState' = [chunkState EXCEPT ![c] = "Done"]
       /\ lastReplayed' =
            [lastReplayed EXCEPT ![Target(c)] = replaySeq[c]]
       /\ emittedGeneration' =
            [emittedGeneration EXCEPT ![c] = capturedGeneration[c]]
       /\ rawResidency' =
            [rawResidency EXCEPT
               ![Target(c)][capturedGeneration[c]] = @ \ {c}]
       /\ trace' = Append(trace, <<c, capturedGeneration[c]>>)
  /\ inFlightChunk' = NoChunk
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, replaySeq, lastQueued, currentGeneration,
                  capturedGeneration, waitState, globalWaitState,
                  rejectedSeqs>>

(* replayRawChunk returned failure; failed_ is the first terminal gate. *)
ObserveReplayFailure ==
  /\ running
  /\ ~poisoned
  /\ ~failureObserved
  /\ inFlightChunk \in Chunks
  /\ failureObserved' = TRUE
  /\ failureCutoff' = nextSeq
  /\ UNCHANGED <<running, poisoned, nextSeq, queue, inFlightChunk, chunkState,
                  replaySeq, lastQueued, lastReplayed, currentGeneration,
                  capturedGeneration, emittedGeneration, rawResidency,
                  waitState, globalWaitState, rejectedSeqs, trace>>

(* Publish poison and stop admission while the failed chunk is still in flight. *)
PublishFailureTerminal ==
  /\ running
  /\ ~poisoned
  /\ failureObserved
  /\ inFlightChunk \in Chunks
  /\ running' = FALSE
  /\ poisoned' = TRUE
  /\ chunkState' =
       [c \in Chunks |->
         IF c \in SeqToSet(queue) THEN "Abandoned" ELSE chunkState[c]]
  /\ rawResidency' =
       [r \in Resources |-> [g \in Generations |->
         rawResidency[r][g] \ SeqToSet(queue)]]
  /\ queue' = <<>>
  /\ UNCHANGED <<failureObserved, failureCutoff, nextSeq, inFlightChunk,
                  replaySeq, lastQueued, lastReplayed, currentGeneration,
                  capturedGeneration, emittedGeneration, waitState,
                  globalWaitState, rejectedSeqs, trace>>

(* Raw cleanup and markReplayDone are the last failed-replay transition. *)
RetireFailedReplay ==
  /\ ~running
  /\ poisoned
  /\ failureObserved
  /\ inFlightChunk \in Chunks
  /\ LET c == inFlightChunk IN
       /\ chunkState' = [chunkState EXCEPT ![c] = "Failed"]
       /\ rawResidency' =
            [rawResidency EXCEPT
               ![Target(c)][capturedGeneration[c]] = @ \ {c}]
  /\ inFlightChunk' = NoChunk
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, replaySeq, lastQueued, lastReplayed,
                  currentGeneration, capturedGeneration, emittedGeneration,
                  waitState, globalWaitState, rejectedSeqs, trace>>

NormalStop ==
  /\ running
  /\ ~failureObserved
  /\ running' = FALSE
  /\ queue' = <<>>
  /\ inFlightChunk' = NoChunk
  /\ rawResidency' = EmptyRawResidency
  /\ chunkState' =
       [c \in Chunks |->
         IF c \in SeqToSet(queue) \/ c = inFlightChunk
         THEN "Abandoned" ELSE chunkState[c]]
  /\ UNCHANGED <<poisoned, failureObserved, failureCutoff, nextSeq, replaySeq,
                  lastQueued, lastReplayed, currentGeneration,
                  capturedGeneration, emittedGeneration, waitState,
                  globalWaitState, rejectedSeqs, trace>>

StartScopedWait(w) ==
  LET r == CoreIdentity(w) IN
  /\ waitState[r] = "Idle"
  /\ lastQueued[r] > lastReplayed[r]
  /\ waitState' = [waitState EXCEPT ![r] = "Waiting"]
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, chunkState, replaySeq, lastQueued,
                  lastReplayed, currentGeneration, capturedGeneration,
                  emittedGeneration, rawResidency, globalWaitState,
                  rejectedSeqs, trace>>

FinishScopedWait(w) ==
  LET r == CoreIdentity(w) IN
  /\ waitState[r] = "Waiting"
  /\ (failureObserved \/ poisoned \/ ~running \/
      lastQueued[r] <= lastReplayed[r])
  /\ waitState' =
       [waitState EXCEPT ![r] =
         IF failureObserved \/ poisoned THEN "Poisoned"
         ELSE IF ~running THEN "Stopped"
         ELSE "CaughtUp"]
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, chunkState, replaySeq, lastQueued,
                  lastReplayed, currentGeneration, capturedGeneration,
                  emittedGeneration, rawResidency, globalWaitState,
                  rejectedSeqs, trace>>

StartGlobalWait ==
  /\ globalWaitState = "Idle"
  /\ Len(queue) > 0 \/ inFlightChunk \in Chunks
  /\ globalWaitState' = "Waiting"
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, chunkState, replaySeq, lastQueued,
                  lastReplayed, currentGeneration, capturedGeneration,
                  emittedGeneration, rawResidency, waitState, rejectedSeqs,
                  trace>>

FinishGlobalWait ==
  /\ globalWaitState = "Waiting"
  /\ (failureObserved \/ poisoned \/ ~running \/
      (Len(queue) = 0 /\ inFlightChunk = NoChunk))
  /\ globalWaitState' =
       IF failureObserved \/ poisoned THEN "Poisoned"
       ELSE IF ~running THEN "Stopped"
       ELSE "CaughtUp"
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, chunkState, replaySeq, lastQueued,
                  lastReplayed, currentGeneration, capturedGeneration,
                  emittedGeneration, rawResidency, waitState, rejectedSeqs,
                  trace>>

(* A DISCARD may select only a backing with no queued or in-flight lease. *)
DiscardRename(r, g) ==
  /\ rawResidency[r][g] = {}
  /\ currentGeneration' = [currentGeneration EXCEPT ![r] = g]
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, nextSeq,
                  queue, inFlightChunk, chunkState, replaySeq, lastQueued,
                  lastReplayed, capturedGeneration, emittedGeneration,
                  rawResidency, waitState, globalWaitState, rejectedSeqs,
                  trace>>

(* OFF mode performs the same capture and replay trace without a queue. *)
InlineReplay(c) ==
  /\ running
  /\ ~poisoned
  /\ ~failureObserved
  /\ Len(queue) = 0
  /\ inFlightChunk = NoChunk
  /\ chunkState[c] = "New"
  /\ LET r == Target(c) IN
       /\ chunkState' = [chunkState EXCEPT ![c] = "Done"]
       /\ replaySeq' = [replaySeq EXCEPT ![c] = nextSeq]
       /\ lastQueued' = [lastQueued EXCEPT ![r] = nextSeq]
       /\ lastReplayed' = [lastReplayed EXCEPT ![r] = nextSeq]
       /\ capturedGeneration' =
            [capturedGeneration EXCEPT ![c] = currentGeneration[r]]
       /\ emittedGeneration' =
            [emittedGeneration EXCEPT ![c] = currentGeneration[r]]
       /\ trace' = Append(trace, <<c, currentGeneration[r]>>)
  /\ nextSeq' = nextSeq + 1
  /\ UNCHANGED <<running, poisoned, failureObserved, failureCutoff, queue,
                  inFlightChunk, currentGeneration, waitState, rawResidency,
                  globalWaitState, rejectedSeqs>>

Next ==
  \/ \E c \in Chunks : AcceptPush(c)
  \/ \E c \in Chunks : PublishThenRefusedPush(c)
  \/ StartReplay
  \/ FinishReplaySuccess
  \/ ObserveReplayFailure
  \/ PublishFailureTerminal
  \/ RetireFailedReplay
  \/ NormalStop
  \/ \E w \in BufferAliases : StartScopedWait(w)
  \/ \E w \in BufferAliases : FinishScopedWait(w)
  \/ StartGlobalWait
  \/ FinishGlobalWait
  \/ \E r \in Resources, g \in Generations : DiscardRename(r, g)
  \/ \E c \in Chunks : InlineReplay(c)

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(StartReplay)
  /\ WF_vars(FinishReplaySuccess)
  /\ WF_vars(PublishFailureTerminal)
  /\ WF_vars(RetireFailedReplay)
  /\ \A w \in BufferAliases : WF_vars(FinishScopedWait(w))
  /\ WF_vars(FinishGlobalWait)

TypeOK ==
  /\ running \in BOOLEAN
  /\ poisoned \in BOOLEAN
  /\ failureObserved \in BOOLEAN
  /\ failureCutoff \in Nat
  /\ nextSeq \in Nat \ {0}
  /\ queue \in Seq(Chunks)
  /\ inFlightChunk \in (Chunks \cup {NoChunk})
  /\ chunkState \in [Chunks -> ChunkStates]
  /\ replaySeq \in [Chunks -> Nat]
  /\ lastQueued \in [Resources -> Nat]
  /\ lastReplayed \in [Resources -> Nat]
  /\ currentGeneration \in [Resources -> Generations]
  /\ capturedGeneration \in [Chunks -> (Generations \cup {NoGeneration})]
  /\ emittedGeneration \in [Chunks -> (Generations \cup {NoGeneration})]
  /\ rawResidency \in [Resources -> [Generations -> SUBSET Chunks]]
  /\ waitState \in [Resources -> WaitStates]
  /\ globalWaitState \in WaitStates
  /\ rejectedSeqs \subseteq Nat
  /\ trace \in Seq(Chunks \X Generations)

ReplayedLeQueued ==
  \A r \in Resources : lastReplayed[r] <= lastQueued[r]

NoRejectedWatermark ==
  /\ \A c \in Chunks : chunkState[c] = "Refused" => replaySeq[c] = 0
  /\ \A r \in Resources : lastQueued[r] \notin rejectedSeqs

ScopedReturnSafe ==
  \A r \in Resources :
    waitState[r] = "CaughtUp" => lastQueued[r] <= lastReplayed[r]

GlobalReturnSafe ==
  globalWaitState = "CaughtUp" =>
    Len(queue) = 0 /\ inFlightChunk = NoChunk

StopUnblocksWithoutSuccess ==
  /\ \A r \in Resources :
       waitState[r] = "Stopped" => ~running /\ ~poisoned
  /\ \A r \in Resources :
       waitState[r] = "Poisoned" => failureObserved \/ poisoned

QueuedChunkUsesCapturedGeneration ==
  \A c \in Chunks :
    chunkState[c] = "Done" => emittedGeneration[c] = capturedGeneration[c]

OneGenerationPerRawChunk ==
  \A c \in Chunks :
    chunkState[c] \in {"Queued", "InFlight", "Done", "Failed", "Abandoned"} =>
      capturedGeneration[c] \in Generations

FailedNeverAcknowledged ==
  \A c \in Chunks :
    chunkState[c] \in {"Failed", "Abandoned"} =>
      lastReplayed[Target(c)] < replaySeq[c]

RawEntryImmutable ==
  \A c \in Chunks :
    chunkState[c] \in {"Queued", "InFlight"} =>
      replaySeq[c] > 0 /\ capturedGeneration[c] \in Generations

RawResidencyMatchesOutstanding ==
  \A r \in Resources, g \in Generations :
    rawResidency[r][g] =
      {c \in Chunks :
        chunkState[c] \in {"Queued", "InFlight"} /\
        Target(c) = r /\ capturedGeneration[c] = g}

InFlightRetainsResidency ==
  inFlightChunk \in Chunks =>
    inFlightChunk \in
      rawResidency[Target(inFlightChunk)][capturedGeneration[inFlightChunk]]

TerminalPrecedesCompletion ==
  /\ failureObserved /\ ~poisoned => inFlightChunk \in Chunks
  /\ \A c \in Chunks :
       chunkState[c] = "Failed" => failureObserved /\ poisoned /\ ~running

FailureBlocksAdmission ==
  failureObserved =>
    /\ failureCutoff > 0
    /\ \A c \in Chunks :
         replaySeq[c] = 0 \/ replaySeq[c] < failureCutoff

InlineTraceEquivalent ==
  \A c \in Chunks : chunkState[c] = "Done" =>
    <<c, capturedGeneration[c]>> \in SeqToSet(trace)

UnrelatedResourceDoesNotBlock ==
  \A r \in Resources :
    (waitState[r] = "Waiting" /\ lastQueued[r] <= lastReplayed[r]) =>
      ENABLED FinishScopedWait(Representative(r))

ScopedWaitEventuallyReturnsOrStopsOrPoisons ==
  \A r \in Resources :
    waitState[r] = "Waiting" ~>
      waitState[r] \in {"CaughtUp", "Stopped", "Poisoned"}

GlobalWaitEventuallyReturnsOrStopsOrPoisons ==
  globalWaitState = "Waiting" ~>
    globalWaitState \in {"CaughtUp", "Stopped", "Poisoned"}

====
