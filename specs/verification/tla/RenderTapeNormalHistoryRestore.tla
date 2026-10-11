---- MODULE RenderTapeNormalHistoryRestore ----
EXTENDS Naturals, TLC

\* Cold queue transaction only. Native tests separately bind original identity,
\* alias mapping, live handle generations and each allocation failure site.
\* No GPU contents, shader coverage or Normal replay qualification is modeled.
CONSTANTS SkipBoundary, EagerMutation
VARIABLES phase, epoch, published, completed, planEpoch, planPrefix,
          touched, discard, safeCommit, attempts
vars == <<phase, epoch, published, completed, planEpoch, planPrefix,
          touched, discard, safeCommit, attempts>>

Init == /\ phase = "New"
        /\ epoch = 1 /\ published = 0 /\ completed = 0
        /\ planEpoch = 0 /\ planPrefix = 0
        /\ touched = {2} /\ discard = FALSE
        /\ safeCommit = TRUE /\ attempts = 0

Prepare == /\ phase = "New"
           /\ phase' = "Prepared"
           /\ planEpoch' = epoch /\ planPrefix' = published
           /\ UNCHANGED <<epoch, published, completed, touched, discard, safeCommit, attempts>>

Publish == /\ phase \in {"New", "Prepared"} /\ published = 0
           /\ published' = 1
           /\ UNCHANGED <<phase, epoch, completed, planEpoch, planPrefix,
                          touched, discard, safeCommit, attempts>>
Complete == /\ phase \in {"New", "Prepared"} /\ completed < published
            /\ completed' = published
            /\ UNCHANGED <<phase, epoch, published, planEpoch, planPrefix,
                           touched, discard, safeCommit, attempts>>
Reset == /\ phase \in {"New", "Prepared"} /\ epoch = 1
         /\ epoch' = 2
         /\ UNCHANGED <<phase, published, completed, planEpoch, planPrefix,
                        touched, discard, safeCommit, attempts>>

Eligible == planEpoch = epoch /\ planPrefix = published /\ completed = published

Attempt == /\ phase = "Prepared"
           /\ attempts' = attempts + 1
           /\ phase' = IF Eligible \/ SkipBoundary THEN "Locked" ELSE "Rejected"
           /\ safeCommit' = Eligible
           /\ UNCHANGED <<epoch, published, completed, planEpoch, planPrefix, touched, discard>>
\* Locked includes off-state allocation and commit; no publisher or reset may
\* interleave here. This is the actual queue lock's linearization boundary.
Stage == /\ phase = "Locked" /\ phase' = "Staged"
         /\ touched' = IF EagerMutation THEN {1} ELSE touched
         /\ UNCHANGED <<epoch, published, completed, planEpoch, planPrefix,
                        discard, safeCommit, attempts>>
AllocationFailure == /\ phase \in {"Locked", "Staged"} /\ phase' = "Rejected"
                     /\ UNCHANGED <<epoch, published, completed, planEpoch, planPrefix,
                                    touched, discard, safeCommit, attempts>>
Commit == /\ phase = "Staged" /\ phase' = "Committed"
          /\ touched' = {1} /\ discard' = TRUE
          /\ UNCHANGED <<epoch, published, completed, planEpoch, planPrefix, safeCommit, attempts>>

Next == Prepare \/ Publish \/ Complete \/ Reset \/ Attempt \/ Stage \/ AllocationFailure \/ Commit
Spec == Init /\ [][Next]_vars
SafeCommit == phase = "Committed" => safeCommit
AtomicRestore == IF phase = "Committed" THEN touched = {1} /\ discard
                 ELSE touched = {2} /\ ~discard
ExactlyOnce == attempts <= 1
Inv == SafeCommit /\ AtomicRestore /\ ExactlyOnce
====
