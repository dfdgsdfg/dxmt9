------------------------- MODULE TracingBatchTransport -------------------------
EXTENDS Naturals, FiniteSets, TLC

(* Executable binding: tracing/batch_transport.hpp. Publication exposes only
   initialized records; recycling follows synchronous persistence; deadline
   expiry retains storage and blocks generation reuse until all owners finish.
   Finite safety model, not a filesystem progress or performance claim. *)
CONSTANT Mode
Blocks == {1, 2}
States == {"Free", "ProducerOwned", "Published", "ConsumerOwned"}
VARIABLES state, initialized, persisted, blockGeneration, generation,
          closed, timedout, stale, earlyRead, earlyRecycle
vars == <<state, initialized, persisted, blockGeneration, generation,
          closed, timedout, stale, earlyRead, earlyRecycle>>
Init ==
  /\ state = [b \in Blocks |-> "Free"]
  /\ initialized = [b \in Blocks |-> FALSE]
  /\ persisted = [b \in Blocks |-> FALSE]
  /\ blockGeneration = [b \in Blocks |-> 1]
  /\ generation = 1
  /\ closed = FALSE
  /\ timedout = FALSE
  /\ stale = FALSE
  /\ earlyRead = FALSE
  /\ earlyRecycle = FALSE
Claim(b) ==
  /\ ~closed /\ state[b] = "Free"
  /\ state' = [state EXCEPT ![b] = "ProducerOwned"]
  /\ initialized' = [initialized EXCEPT ![b] = FALSE]
  /\ persisted' = [persisted EXCEPT ![b] = FALSE]
  /\ blockGeneration' = [blockGeneration EXCEPT ![b] = generation]
  /\ UNCHANGED <<generation, closed, timedout, stale, earlyRead, earlyRecycle>>
Write(b) ==
  /\ state[b] = "ProducerOwned" /\ ~initialized[b]
  /\ initialized' = [initialized EXCEPT ![b] = TRUE]
  /\ UNCHANGED <<state, persisted, blockGeneration, generation, closed,
                 timedout, stale, earlyRead, earlyRecycle>>
Publish(b) ==
  /\ state[b] = "ProducerOwned"
  /\ (initialized[b] \/ Mode = "EarlyPublication")
  /\ state' = [state EXCEPT ![b] = "Published"]
  /\ UNCHANGED <<initialized, persisted, blockGeneration, generation, closed,
                 timedout, stale, earlyRead, earlyRecycle>>
Take(b) ==
  /\ state[b] = "Published"
  /\ state' = [state EXCEPT ![b] = "ConsumerOwned"]
  /\ earlyRead' = (earlyRead \/ ~initialized[b])
  /\ UNCHANGED <<initialized, persisted, blockGeneration, generation, closed,
                 timedout, stale, earlyRecycle>>
Persist(b) ==
  /\ state[b] = "ConsumerOwned" /\ ~persisted[b]
  /\ persisted' = [persisted EXCEPT ![b] = TRUE]
  /\ UNCHANGED <<state, initialized, blockGeneration, generation, closed,
                 timedout, stale, earlyRead, earlyRecycle>>
Recycle(b) ==
  /\ state[b] = "ConsumerOwned"
  /\ (persisted[b] \/ Mode = "EarlyRecycle")
  /\ state' = [state EXCEPT ![b] = "Free"]
  /\ earlyRecycle' = (earlyRecycle \/ ~persisted[b])
  /\ UNCHANGED <<initialized, persisted, blockGeneration, generation, closed,
                 timedout, stale, earlyRead>>
Close ==
  /\ ~closed /\ closed' = TRUE
  /\ UNCHANGED <<state, initialized, persisted, blockGeneration, generation,
                 timedout, stale, earlyRead, earlyRecycle>>
Deadline ==
  /\ closed /\ ~timedout /\ timedout' = TRUE
  /\ UNCHANGED <<state, initialized, persisted, blockGeneration, generation,
                 closed, stale, earlyRead, earlyRecycle>>
Restart ==
  /\ closed /\ generation = 1
  /\ ((\A b \in Blocks : state[b] = "Free") \/ Mode = "LiveReuse")
  /\ generation' = 2
  /\ closed' = FALSE /\ timedout' = FALSE
  /\ stale' = (stale \/ (\E b \in Blocks : state[b] # "Free"))
  /\ UNCHANGED <<state, initialized, persisted, blockGeneration,
                 earlyRead, earlyRecycle>>
Next == (\E b \in Blocks : Claim(b) \/ Write(b) \/ Publish(b) \/ Take(b) \/
                            Persist(b) \/ Recycle(b)) \/ Close \/ Deadline \/ Restart
TypeOK == /\ state \in [Blocks -> States]
          /\ initialized \in [Blocks -> BOOLEAN]
          /\ persisted \in [Blocks -> BOOLEAN]
          /\ blockGeneration \in [Blocks -> {1, 2}]
          /\ generation \in {1, 2}
NoUninitializedRead == ~earlyRead
NoRecycleBeforePersistence == ~earlyRecycle
NoLiveGenerationReuse == ~stale
LiveBlocksHaveCurrentGeneration ==
  \A b \in Blocks : state[b] # "Free" => blockGeneration[b] = generation
Spec == Init /\ [][Next]_vars
=============================================================================
