---- MODULE PsoSlotPublication ----
(******************************************************************************
 * Bounded model of the draw-PSO segmented slot table.
 *
 * `next` is the append cursor, `staged` is the one writer's not-yet-published
 * slot, and `published` is the acquire-visible prefix.  The concrete table
 * uses one release store per slot followed by a release size publication;
 * readers reject every index outside that prefix and never observe `values`
 * for an unpublished slot.
 ******************************************************************************)
EXTENDS Naturals, FiniteSets, Integers

CONSTANTS MaxSlot, Generations, UnsafeLookup
ASSUME MaxSlot \in Nat
ASSUME Generations # {}

Slots == 0..MaxSlot
None == -1

VARIABLES next, values, published, staged, observedSlot, observedValue,
          publishedValues
vars == <<next, values, published, staged, observedSlot, observedValue,
          publishedValues>>

Init ==
  /\ next = 0
  /\ values = [i \in Slots |-> 0]
  /\ published = {}
  /\ staged = None
  /\ observedSlot = None
  /\ observedValue = 0
  /\ publishedValues = [i \in Slots |-> 0]

Append(g) ==
  /\ staged = None
  /\ next <= MaxSlot
  /\ g \in Generations
  /\ values' = [values EXCEPT ![next] = g]
  /\ staged' = next
  /\ UNCHANGED <<next, published, observedSlot, observedValue,
                 publishedValues>>

Publish ==
  /\ staged # None
  /\ published' = published \cup {staged}
  /\ publishedValues' = [publishedValues EXCEPT ![staged] = values[staged]]
  /\ next' = next + 1
  /\ staged' = None
  /\ UNCHANGED <<values, observedSlot, observedValue>>

Lookup(i) ==
  /\ i \in Slots
  /\ observedSlot' = i
  /\ observedValue' =
       IF i \in published \/ (UnsafeLookup /\ i = staged)
       THEN values[i]
       ELSE 0
  /\ UNCHANGED <<next, values, published, staged, publishedValues>>

Next ==
  (\E g \in Generations : Append(g))
  \/ Publish
  \/ (\E i \in Slots : Lookup(i))

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ next \in 0..(MaxSlot + 1)
  /\ values \in [Slots -> Nat]
  /\ published \subseteq Slots
  /\ staged \in Slots \cup {None}
  /\ observedSlot \in Slots \cup {None}
  /\ observedValue \in Nat
  /\ publishedValues \in [Slots -> Nat]

AppendOnly ==
  published = {i \in Slots : i < next}

PublishedValuesValid ==
  \A i \in published : values[i] \in Generations /\ publishedValues[i] = values[i]

LookupFailClosed ==
  observedSlot = None \/ observedValue = 0 \/
    /\ observedSlot \in published
    /\ observedValue = publishedValues[observedSlot]

NoUnpublishedValue ==
  observedSlot = None \/ observedSlot \in published \/ observedValue = 0

Safety == TypeOK /\ AppendOnly /\ PublishedValuesValid /\ LookupFailClosed
                         /\ NoUnpublishedValue

====
