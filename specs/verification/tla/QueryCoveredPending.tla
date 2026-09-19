---- MODULE QueryCoveredPending ----
EXTENDS Naturals, TLC
(* Pending-only refinement. Independent stale-low progress/coverage reads;
   terminal and epoch sampled at the decision linearization point. C++ cache
   fairness and driver error discovery are not modeled. Existing publication
   model owns eventual submission/completion under fairness. *)
CONSTANT Mutation
VARIABLE s
vars == <<s>>
Init == s = [requested |-> 0, woken |-> 0, covered |-> 0,
  completed |-> 0, uploads |-> 0, certUpload |-> 0,
  terminal |-> FALSE, epoch |-> 1, phase |-> "Idle",
  readCoverage |-> 0, readCompleted |-> 0, readUpload |-> 0,
  accepted |-> FALSE, seenTerminal |-> FALSE, seenEpoch |-> 1,
  seenRequest |-> 0, seenWake |-> 0, seenUpload |-> 0]
Request == /\ s.requested = 0 /\ ~s.terminal
           /\ s' = [s EXCEPT !.requested = 1]
Wake == /\ s.requested = 1 /\ s.woken = 0
        /\ s' = [s EXCEPT !.woken = 1]
Grant == /\ s.covered = 0
         /\ (Mutation = "EarlyRequest" \/ s.requested = 1)
         /\ (Mutation \in {"EarlyRequest", "EarlyWake"} \/ s.woken = 1)
         /\ s' = [s EXCEPT !.covered = 1]
Upload == /\ s.uploads = 0 /\ s' = [s EXCEPT !.uploads = 1]
Certify == /\ s.certUpload = 0
           /\ (Mutation = "Upload" \/ s.uploads = 1)
           /\ s' = [s EXCEPT !.certUpload = 1]
Complete == /\ s.woken = 1 /\ s.completed = 0
            /\ s' = [s EXCEPT !.completed = 1]
Fail == /\ ~s.terminal /\ s' = [s EXCEPT !.terminal = TRUE]
Reset == /\ s.epoch = 1 /\ s' = [s EXCEPT !.epoch = 2]
Read == /\ s.phase = "Idle"
        /\ \E c \in 0..s.covered, d \in 0..s.completed, u \in 0..s.certUpload:
          s' = [s EXCEPT !.phase = "Read", !.readCoverage = c,
                !.readCompleted = d, !.readUpload = u]
Decide == /\ s.phase = "Read"
          /\ s' = [s EXCEPT !.phase = "Done",
             !.accepted = (s.readCoverage = 1 /\ s.readCompleted = 0 /\
               s.readUpload = 1 /\ (Mutation = "Terminal" \/ ~s.terminal) /\
               (Mutation = "Identity" \/ s.epoch = 1)),
             !.seenTerminal = s.terminal, !.seenEpoch = s.epoch,
             !.seenRequest = s.requested, !.seenWake = s.woken,
             !.seenUpload = s.uploads]
Next == Request \/ Wake \/ Grant \/ Upload \/ Certify \/ Complete \/
        Fail \/ Reset \/ Read \/ Decide
Spec == Init /\ [][Next]_vars
CoverageSound == s.covered <= s.requested /\ s.covered <= s.woken
UploadSound == s.certUpload <= s.uploads
PendingSound == s.accepted => /\ ~s.seenTerminal /\ s.seenEpoch = 1
  /\ s.seenRequest = 1 /\ s.seenWake = 1 /\ s.seenUpload = 1
====
