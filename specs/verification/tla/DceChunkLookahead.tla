---- MODULE DceChunkLookahead ----
EXTENDS Integers, Naturals, Sequences, TLC

(*
 * One-next-source FrameGraph DCE scheduling refinement.
 *
 * C++ mapping:
 *   ready        -> CommandQueue::readySlots_
 *   held         -> runDceChunkLookaheadEncodeLoop::held
 *   prefixEncoded
 *                -> encodedPrefix / prefixSession
 *   submitted    -> FIFO QueueSubmissionRecord order
 *   stopped      -> CommandQueue::stop_
 *
 * UseReady submits N and carries already-dequeued N+1 as the next held source.
 * FailOpen submits N without proof whenever no successor is already ready
 * after optional prefix encode. Neither action waits, merges completion
 * sources, or moves commands across a chunk boundary.
 *)

CONSTANTS MaxSources, MaxInflight

VARIABLES nextToPublish, ready, held, prefixEncoded, submitted, completed,
          stopped

vars == <<nextToPublish, ready, held, prefixEncoded, submitted, completed,
          stopped>>

Inflight ==
  Len(ready) +
  (IF held = 0 THEN 0 ELSE 1) +
  Len(submitted) - completed

Init ==
  /\ nextToPublish = 1
  /\ ready = <<>>
  /\ held = 0
  /\ prefixEncoded = FALSE
  /\ submitted = <<>>
  /\ completed = 0
  /\ stopped = FALSE

Publish ==
  /\ ~stopped
  /\ nextToPublish <= MaxSources
  /\ Inflight < MaxInflight
  /\ ready' = Append(ready, nextToPublish)
  /\ nextToPublish' = nextToPublish + 1
  /\ UNCHANGED <<held, prefixEncoded, submitted, completed,
                 stopped>>

Dequeue ==
  /\ held = 0
  /\ Len(ready) > 0
  /\ held' = Head(ready)
  /\ prefixEncoded' = FALSE
  /\ ready' = Tail(ready)
  /\ UNCHANGED <<nextToPublish, submitted, completed,
                 stopped>>

EncodePrefix ==
  /\ held # 0
  /\ ~prefixEncoded
  /\ prefixEncoded' = TRUE
  /\ UNCHANGED <<nextToPublish, ready, held, submitted, completed,
                 stopped>>

UseReady ==
  /\ held # 0
  /\ Len(ready) > 0
  /\ submitted' = Append(submitted, held)
  /\ held' = Head(ready)
  /\ prefixEncoded' = FALSE
  /\ ready' = Tail(ready)
  /\ UNCHANGED <<nextToPublish, completed, stopped>>

FailOpen ==
  /\ held # 0
  /\ Len(ready) = 0
  /\ submitted' = Append(submitted, held)
  /\ held' = 0
  /\ prefixEncoded' = FALSE
  /\ UNCHANGED <<nextToPublish, ready, completed,
                 stopped>>

Complete ==
  /\ completed < Len(submitted)
  /\ completed' = completed + 1
  /\ UNCHANGED <<nextToPublish, ready, held, prefixEncoded, submitted,
                 stopped>>

Stop ==
  /\ ~stopped
  /\ stopped' = TRUE
  /\ UNCHANGED <<nextToPublish, ready, held, prefixEncoded, submitted,
                 completed>>

Quiesce ==
  /\ stopped
  /\ held = 0
  /\ Len(ready) = 0
  /\ completed = Len(submitted)
  /\ UNCHANGED vars

Next ==
  Publish \/ Dequeue \/ EncodePrefix \/ UseReady \/ FailOpen \/ Complete \/
  Stop \/ Quiesce

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ nextToPublish \in 1..(MaxSources + 1)
  /\ ready \in Seq(1..MaxSources)
  /\ held \in 0..MaxSources
  /\ prefixEncoded \in BOOLEAN
  /\ submitted \in Seq(1..MaxSources)
  /\ completed \in 0..Len(submitted)
  /\ stopped \in BOOLEAN

SubmittedPrefix ==
  \A i \in 1..Len(submitted): submitted[i] = i

HeldIsNext ==
  held = 0 \/ held = Len(submitted) + 1

ReadyIsFollowingPrefix ==
  \A i \in 1..Len(ready):
    ready[i] = Len(submitted) + (IF held = 0 THEN 0 ELSE 1) + i

PublishedPartition ==
  nextToPublish =
    Len(submitted) + (IF held = 0 THEN 0 ELSE 1) + Len(ready) + 1

BoundedHold ==
  held = 0 \/ held \in 1..MaxSources

PrefixOwnedByHeld ==
  ~prefixEncoded \/ held # 0

CompletionPrefix ==
  completed <= Len(submitted)

Safety ==
  TypeOK /\ SubmittedPrefix /\ HeldIsNext /\ ReadyIsFollowingPrefix /\
  PublishedPartition /\ BoundedHold /\ PrefixOwnedByHeld /\ CompletionPrefix

====
