---- MODULE RenderTapeGroupedPublication ----
(******************************************************************************
 * Cold CPU render-tape grouped publication model.
 *
 * Tape events 5 and 6 append seven and two ordered semantic draws to one
 * writing source. Event 5 does not publish. Event 6 takes either legal direct
 * or compatibility capacity fallback, appends the whole nine-draw prefix,
 * and publishes one qualified source range. This model deliberately excludes
 * the product queue, Pool residency, Metal execution, GPU progress, and
 * liveness: those obligations have separate owners.
 ******************************************************************************)

EXTENDS Naturals, Sequences, TLC

CONSTANTS AllowEarlyPublish, DropBDraw, AllowWrongFirstEvent

Routes == {"Direct", "Compatibility"}
DrawA == <<"A1", "A2", "A3", "A4", "A5", "A6", "A7">>
DrawB == <<"B1", "B2">>
ExpectedDraws == DrawA \o DrawB
ExpectedEvents == <<5, 6>>
Plan == [firstEvent   |-> 5,
         lastEvent    |-> 6,
         drawCount    |-> 9,
         releaseEvent |-> 8]

NoSource == [kind        |-> "None",
             firstEvent  |-> 0,
             lastEvent   |-> 0,
             drawCount   |-> 0,
             route       |-> "None"]

PublishedSource(firstEvent, lastEvent, drawCount, route) ==
  [kind        |-> "Actual",
   firstEvent  |-> firstEvent,
   lastEvent   |-> lastEvent,
   drawCount   |-> drawCount,
   route       |-> route]

VARIABLES phase,
          observedEvents,
          observedDraws,
          source,
          capacityBranch,
          rotated,
          released,
          releaseEvent,
          failed

vars == <<phase, observedEvents, observedDraws, source, capacityBranch,
          rotated, released, releaseEvent, failed>>

Init ==
  /\ phase = "AppendA"
  /\ observedEvents = <<>>
  /\ observedDraws = <<>>
  /\ source = NoSource
  /\ capacityBranch = "None"
  /\ rotated = FALSE
  /\ released = FALSE
  /\ releaseEvent = 0
  /\ failed = FALSE

AppendA ==
  /\ phase = "AppendA" /\ ~failed
  /\ observedEvents' = Append(observedEvents, 5)
  /\ observedDraws' = DrawA
  /\ phase' = "AppendB"
  /\ UNCHANGED <<source, capacityBranch, rotated, released, releaseEvent,
                  failed>>

AppendB ==
  /\ phase = "AppendB" /\ ~failed
  /\ observedEvents' = Append(observedEvents, 6)
  /\ observedDraws' = IF DropBDraw THEN observedDraws ELSE ExpectedDraws
  /\ capacityBranch' \in Routes
  /\ phase' = "Publish"
  /\ UNCHANGED <<source, rotated, released, releaseEvent, failed>>

Publish ==
  /\ phase = "Publish" /\ ~failed
  /\ source' = PublishedSource(
        IF AllowWrongFirstEvent THEN 6 ELSE Plan.firstEvent,
        Plan.lastEvent,
        Len(observedDraws),
        capacityBranch)
  /\ phase' = "Release"
  /\ UNCHANGED <<observedEvents, observedDraws, capacityBranch,
                  rotated, released, releaseEvent, failed>>

BugEarlyPublish ==
  /\ AllowEarlyPublish
  /\ phase = "AppendB" /\ ~failed
  /\ source' = PublishedSource(5, 5, Len(observedDraws), "Early")
  /\ phase' = "Release"
  /\ UNCHANGED <<observedEvents, observedDraws, capacityBranch,
                  rotated, released, releaseEvent, failed>>

Release ==
  /\ phase = "Release" /\ ~failed
  /\ released' = TRUE
  /\ releaseEvent' = Plan.releaseEvent
  /\ phase' = "Finished"
  /\ UNCHANGED <<observedEvents, observedDraws, source, capacityBranch,
                  rotated, failed>>

FailTerminal ==
  /\ phase \in {"AppendA", "AppendB", "Publish", "Release"}
  /\ ~failed
  /\ phase' = "Failed"
  /\ failed' = TRUE
  /\ UNCHANGED <<observedEvents, observedDraws, source, capacityBranch,
                  rotated, released, releaseEvent>>

Next ==
  \/ AppendA
  \/ AppendB
  \/ Publish
  \/ BugEarlyPublish
  \/ Release
  \/ FailTerminal

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ phase \in {"AppendA", "AppendB", "Publish", "Release", "Finished", "Failed"}
  /\ observedEvents \in Seq(Nat)
  /\ observedDraws \in Seq({"A1", "A2", "A3", "A4", "A5", "A6", "A7", "B1", "B2"})
  /\ source.kind \in {"None", "Actual"}
  /\ source.firstEvent \in Nat
  /\ source.lastEvent \in Nat
  /\ source.drawCount \in Nat
  /\ source.route \in Routes \cup {"None", "Early"}
  /\ capacityBranch \in Routes \cup {"None"}
  /\ rotated = FALSE
  /\ released \in BOOLEAN
  /\ releaseEvent \in Nat
  /\ failed \in BOOLEAN

NoEarlyPublication ==
  source.kind = "None" \/
    /\ source.firstEvent = Plan.firstEvent
    /\ source.lastEvent = Plan.lastEvent
    /\ source.drawCount = Plan.drawCount
    /\ source.route \in Routes

ExactRangeCoverage ==
  source.kind = "None" \/
    /\ observedEvents = ExpectedEvents
    /\ source.firstEvent = Plan.firstEvent
    /\ source.lastEvent = Plan.lastEvent
    /\ source.drawCount = Plan.drawCount
    /\ source.drawCount = Len(observedDraws)

OrderedDrawsExactlyOnce ==
  source.kind = "None" \/ observedDraws = ExpectedDraws

CompletionAfterQualifiedPublish ==
  ~released \/
    /\ source.kind = "Actual"
    /\ source.firstEvent = Plan.firstEvent
    /\ source.lastEvent = Plan.lastEvent
    /\ source.drawCount = Plan.drawCount
    /\ source.route \in Routes
    /\ releaseEvent = Plan.releaseEvent

NoRotationOnCapacity ==
  ~rotated

FailureTerminal ==
  ~failed \/ phase = "Failed"

Invariant ==
  /\ TypeOK
  /\ NoEarlyPublication
  /\ ExactRangeCoverage
  /\ OrderedDrawsExactlyOnce
  /\ CompletionAfterQualifiedPublish
  /\ NoRotationOnCapacity
  /\ FailureTerminal

THEOREM Spec => []Invariant

=============================================================================
