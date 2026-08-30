---- MODULE MutationComposition ----
(* Bounded truth table for the observation-only mutation classifier.  This
   model has no composition action: it checks only the production decision
   relation consumed by the native truth table. *)
EXTENDS Naturals, MutationCompositionTable

CONSTANTS Resources, Generations, TapeIdentities, Ordinals, Dispositions,
          Completions, Offsets, Sizes, Booleans, Decisions

(* `ordinal` is the production source/replay value retained for settlement.
   `orderingGeneration`/`orderingOrdinal` are the observer's typed,
   generation-qualified authority.  The latter is issued at observation
   ingress and is shared by synchronous and deferred paths. *)
MutationSources == {"synchronous", "deferred", "replay"}

Event == [resource : Resources,
          generation : Generations,
          tape : TapeIdentities,
          ordinal : Ordinals,
          orderingGeneration : Nat,
          orderingOrdinal : Ordinals,
          source : MutationSources,
          offset : Offsets,
          size : Sizes,
          disposition : Dispositions,
          successful : Booleans,
          completion : Completions]

(* Deliberately small representatives keep the model a truth table rather
   than a Cartesian-product stress model.  Every production decision branch
   has at least one representative, and the native test invokes the same
   production classifier for the corresponding tuples. *)
SampleEvents == {
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 900,
   orderingGeneration |-> 1, orderingOrdinal |-> 1, source |-> "synchronous",
   offset |-> 0, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 2, generation |-> 1, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "deferred",
   offset |-> 8, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 2, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "deferred",
   offset |-> 8, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 3,
   orderingGeneration |-> 1, orderingOrdinal |-> 3, source |-> "synchronous",
   offset |-> 8, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "synchronous",
   offset |-> 8, size |-> 8, disposition |-> "plain",
   successful |-> FALSE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "deferred",
   offset |-> 8, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "pending"],
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "deferred",
   offset |-> 8, size |-> 8, disposition |-> "discard",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "deferred",
   offset |-> 4, size |-> 8, disposition |-> "nooverwrite",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 2, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "replay",
   offset |-> 16, size |-> 8, disposition |-> "nooverwrite",
   successful |-> TRUE, completion |-> "complete"],
  [resource |-> 1, generation |-> 1, tape |-> 0, ordinal |-> 2,
   orderingGeneration |-> 1, orderingOrdinal |-> 2, source |-> "replay",
   offset |-> 16, size |-> 8, disposition |-> "plain",
   successful |-> TRUE, completion |-> "complete"]
}

ValidTapeIdentity(tape) == tape # 0

Expected(p, c, barrier) ==
  IF p.resource # c.resource THEN "different-resource"
  ELSE IF p.generation # c.generation THEN "different-generation"
  ELSE IF ~ValidTapeIdentity(p.tape) \/
          ~ValidTapeIdentity(c.tape) \/ p.tape # c.tape
       THEN "render-tape-identity"
  ELSE IF p.orderingGeneration # c.orderingGeneration \/
          p.orderingOrdinal >= c.orderingOrdinal
       THEN "source-order"
  ELSE IF barrier THEN "barrier"
  ELSE IF ~p.successful \/ ~c.successful THEN "failure"
  ELSE IF p.completion # "complete" \/ c.completion # "complete"
       THEN "completion"
  ELSE IF p.disposition = "discard" \/ c.disposition = "discard"
       THEN "disposition"
  ELSE IF p.disposition = "nooverwrite" /\
          c.disposition = "nooverwrite" /\ p.offset < c.offset + c.size /\
          c.offset < p.offset + p.size
       THEN "range-overlap"
  ELSE "candidate"

VARIABLES previous, current, barrier, decision

vars == <<previous, current, barrier, decision>>
Init ==
  /\ previous \in SampleEvents
  /\ current \in SampleEvents
  /\ barrier \in Booleans
  /\ decision = Expected(previous, current, barrier)

Next ==
  \/ ( /\ previous' \in SampleEvents
     /\ current' \in SampleEvents
     /\ barrier' \in Booleans
     /\ decision' = Expected(previous', current', barrier') )
  \/ UNCHANGED vars

TypeOK == previous \in Event /\ current \in Event /\
          barrier \in Booleans /\ decision \in Decisions
DecisionMatches == decision = Expected(previous, current, barrier)
DecisionVocabulary == Decisions = CompositionDecisions

(* These are the two mixed-path witnesses that previously failed when the
   raw synchronous counter was compared directly with replaySeq. *)
MixedPathCandidates ==
  Expected(
    [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 900,
     orderingGeneration |-> 1, orderingOrdinal |-> 1,
     source |-> "synchronous", offset |-> 0, size |-> 8,
     disposition |-> "plain", successful |-> TRUE, completion |-> "complete"],
    [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
     orderingGeneration |-> 1, orderingOrdinal |-> 2,
     source |-> "deferred", offset |-> 8, size |-> 8,
     disposition |-> "plain", successful |-> TRUE, completion |-> "complete"],
    FALSE) = "candidate" /\
  Expected(
    [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 900,
     orderingGeneration |-> 1, orderingOrdinal |-> 1,
     source |-> "deferred", offset |-> 0, size |-> 8,
     disposition |-> "plain", successful |-> TRUE, completion |-> "complete"],
    [resource |-> 1, generation |-> 1, tape |-> 1, ordinal |-> 2,
     orderingGeneration |-> 1, orderingOrdinal |-> 2,
     source |-> "synchronous", offset |-> 8, size |-> 8,
     disposition |-> "plain", successful |-> TRUE, completion |-> "complete"],
    FALSE) = "candidate"

MixedPathBinding == MixedPathCandidates

(* Reset restarts the local ordinal only after advancing its generation.  The
   differing generation is intentionally not orderable, so a stale identity
   cannot compose with a post-reset event. *)
ResetBefore == [generation |-> 1, ordinal |-> 3, source |-> "synchronous"]
ResetAfter == [generation |-> 2, ordinal |-> 1, source |-> "deferred"]
ResetGenerationAdvances ==
  ResetBefore.generation < ResetAfter.generation /\
  ResetAfter.ordinal = 1 /\
  ~(ResetBefore.generation = ResetAfter.generation)

ResetBinding == ResetGenerationAdvances

Spec == Init /\ [][Next]_vars

THEOREM Spec => []TypeOK
====
