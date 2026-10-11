---- MODULE RenderTapeNormalSetup ----
EXTENDS Naturals, FiniteSets, TLC
CONSTANTS TemporaryAlias, LateSeed, EarlyArm
VARIABLES stage, owner, handle, restoredHandle, acquired, released,
          seeded, completed, restored, touched
vars == <<stage, owner, handle, restoredHandle, acquired, released,
          seeded, completed, restored, touched>>
Objects == {1, 2}
OriginalTouches == {1}
Init == /\ stage = "Setup"
        /\ owner = [i \in Objects |-> "Absent"]
        /\ handle = [i \in Objects |-> i]
        /\ restoredHandle = [i \in Objects |-> 0]
        /\ acquired = [i \in Objects |-> 0]
        /\ released = [i \in Objects |-> 0]
        /\ seeded = FALSE /\ completed = FALSE /\ restored = FALSE /\ touched = {}
Prepare(i) == /\ stage = "Setup" /\ owner[i] = "Absent"
              /\ owner' = [owner EXCEPT ![i] = "Hidden"]
              /\ acquired' = [acquired EXCEPT ![i] = @ + 1]
              /\ UNCHANGED <<stage, handle, restoredHandle, released,
                             seeded, completed, restored, touched>>
Seed == /\ stage = "Setup" /\ ~seeded
        /\ seeded' = TRUE /\ touched' = Objects
        /\ UNCHANGED <<stage, owner, handle, restoredHandle, acquired, released, completed, restored>>
Checkpoint == /\ stage = "Setup" /\ seeded
              /\ \A i \in Objects : owner[i] = "Hidden"
              /\ completed' = TRUE /\ stage' = "Completed"
              /\ UNCHANGED <<owner, handle, restoredHandle, acquired, released, seeded, restored, touched>>
Restore == /\ stage = "Completed"
           /\ restored' = TRUE /\ touched' = OriginalTouches /\ stage' = "Restored"
           /\ restoredHandle' = handle
           /\ owner' = IF TemporaryAlias THEN [i \in Objects |-> "Dropped"] ELSE owner
           /\ released' = IF TemporaryAlias THEN [i \in Objects |-> 1] ELSE released
           /\ UNCHANGED <<handle, acquired, seeded, completed>>
LateSeedEffect == /\ LateSeed /\ stage = "Restored" /\ touched = OriginalTouches
                  /\ touched' = Objects
                  /\ UNCHANGED <<stage, owner, handle, restoredHandle, acquired, released, seeded, completed, restored>>
Arm == /\ (stage = "Restored" \/ (EarlyArm /\ stage = "Setup"))
       /\ stage' = "Armed"
       /\ UNCHANGED <<owner, handle, restoredHandle, acquired, released, seeded, completed, restored, touched>>
Activate(i) == /\ stage = "Armed" /\ owner[i] \in {"Hidden", "Dropped"}
               /\ owner' = [owner EXCEPT ![i] = "Active"]
               /\ handle' = IF owner[i] = "Dropped" THEN [handle EXCEPT ![i] = @ + 2] ELSE handle
               /\ acquired' = IF owner[i] = "Dropped" THEN [acquired EXCEPT ![i] = @ + 1] ELSE acquired
               /\ UNCHANGED <<stage, restoredHandle, released, seeded, completed, restored, touched>>
\* Cleanup's production drain/physical GPU retirement is an external premise.
\* This model checks only exactly-once wrapper reference ownership.
Cleanup == /\ stage # "Closed" /\ stage' = "Closed"
           /\ owner' = [i \in Objects |-> "Released"]
           /\ released' = acquired
           /\ UNCHANGED <<handle, restoredHandle, acquired, seeded, completed, restored, touched>>
Next == (\E i \in Objects : Prepare(i) \/ Activate(i)) \/ Seed \/ Checkpoint \/ Restore \/ LateSeedEffect \/ Arm \/ Cleanup
Spec == Init /\ [][Next]_vars
ArmAfterRestoration == stage = "Armed" => completed /\ restored
OriginalHistory == stage \in {"Restored", "Armed"} => touched = OriginalTouches
StableAliasIdentity == stage = "Armed" => \A i \in Objects : owner[i] = "Active" => handle[i] = restoredHandle[i]
ReferenceConservation == \A i \in Objects :
  acquired[i] = released[i] + (IF owner[i] \in {"Hidden", "Active"} THEN 1 ELSE 0)
Inv == ArmAfterRestoration /\ OriginalHistory /\ StableAliasIdentity /\ ReferenceConservation
====
