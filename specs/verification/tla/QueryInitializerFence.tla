---- MODULE QueryInitializerFence ----
EXTENDS Naturals, TLC

(* Product refinement of an ordinary published prefix and an independent
   initializer prefix. A signal is not successful command-buffer completion.
   Native binding: query::classify, initializer upload admission/reaping.
   Excludes Metal bytes, C++ memory order and ordinary queue capacity. *)
CONSTANTS MaxSeq, MaxUpload, Capacity, Mutation
VARIABLES published, completed, uploaded, signaled, succeeded, failed,
          issued, seq, upload, ok, seenDone, seenSignal, seenSuccess,
          seenSeq, seenUpload, seenFailure
vars == <<published, completed, uploaded, signaled, succeeded, failed,
          issued, seq, upload, ok, seenDone, seenSignal, seenSuccess,
          seenSeq, seenUpload, seenFailure>>
Init ==
  /\ published = 0 /\ completed = 0 /\ uploaded = 0
  /\ signaled = 0 /\ succeeded = 0 /\ failed = FALSE
  /\ issued = FALSE /\ seq = 0 /\ upload = 0 /\ ok = FALSE
  /\ seenDone = 0 /\ seenSignal = 0 /\ seenSuccess = 0
  /\ seenSeq = 0 /\ seenUpload = 0 /\ seenFailure = FALSE
KeepObservation == UNCHANGED <<ok, seenDone, seenSignal, seenSuccess,
                               seenSeq, seenUpload, seenFailure>>
Publish ==
  /\ ~failed /\ published < MaxSeq /\ published' = published + 1
  /\ UNCHANGED <<completed, uploaded, signaled, succeeded, failed, issued, seq, upload>>
  /\ KeepObservation
Complete ==
  /\ ~failed /\ completed < published /\ completed' = completed + 1
  /\ UNCHANGED <<published, uploaded, signaled, succeeded, failed, issued, seq, upload>>
  /\ KeepObservation
Upload ==
  /\ ~failed /\ uploaded < MaxUpload
  /\ (Mutation = "OmitCapacity" \/ uploaded - succeeded < Capacity)
  /\ uploaded' = uploaded + 1
  /\ UNCHANGED <<published, completed, signaled, succeeded, failed, issued, seq, upload>>
  /\ KeepObservation
Signal ==
  /\ ~failed /\ signaled < uploaded /\ signaled' = signaled + 1
  /\ UNCHANGED <<published, completed, uploaded, succeeded, failed, issued, seq, upload>>
  /\ KeepObservation
Succeed ==
  /\ ~failed /\ succeeded < uploaded /\ succeeded' = succeeded + 1
  /\ UNCHANGED <<published, completed, uploaded, signaled, failed, issued, seq, upload>>
  /\ KeepObservation
End ==
  /\ ~failed /\ issued' = TRUE /\ seq' = published /\ upload' = uploaded
  /\ UNCHANGED <<published, completed, uploaded, signaled, succeeded, failed>>
  /\ KeepObservation
Poll ==
  /\ issued
  /\ ok' = /\ ~failed /\ completed >= seq
            /\ (Mutation = "IgnoreUpload" \/
                (signaled >= upload /\ (Mutation = "SignalOnly" \/ succeeded >= upload)))
  /\ seenDone' = completed /\ seenSignal' = signaled /\ seenSuccess' = succeeded
  /\ seenSeq' = seq /\ seenUpload' = upload /\ seenFailure' = failed
  /\ UNCHANGED <<published, completed, uploaded, signaled, succeeded, failed, issued, seq, upload>>
Fail ==
  /\ ~failed /\ failed' = TRUE
  /\ UNCHANGED <<published, completed, uploaded, signaled, succeeded, issued, seq, upload>>
  /\ KeepObservation
Next == Publish \/ Complete \/ Upload \/ Signal \/ Succeed \/ End \/ Poll \/ Fail
Spec == Init /\ [][Next]_vars /\ WF_vars(Complete) /\ WF_vars(Signal) /\ WF_vars(Succeed)
TypeOK ==
  /\ published \in 0..MaxSeq /\ completed \in 0..published /\ seq \in 0..published
  /\ uploaded \in 0..MaxUpload /\ upload \in 0..uploaded
  /\ signaled \in 0..uploaded /\ succeeded \in 0..uploaded
  /\ failed \in BOOLEAN /\ issued \in BOOLEAN /\ ok \in BOOLEAN
CapacitySafety == uploaded - succeeded <= Capacity
CompletionSafety == ok => /\ ~seenFailure /\ seenDone >= seenSeq
                          /\ seenSignal >= seenUpload /\ seenSuccess >= seenUpload
Progress == \A s \in 0..MaxSeq, u \in 0..MaxUpload:
  (published >= s /\ uploaded >= u /\ ~failed) ~>
    ((completed >= s /\ signaled >= u /\ succeeded >= u) \/ failed)
====
