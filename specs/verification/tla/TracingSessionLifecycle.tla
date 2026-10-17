----------------------------- MODULE TracingSessionLifecycle -----------------------------
EXTENDS Naturals, TLC

(***************************************************************************)
(* Bounded reference model for the process-wide diagnostic tracing session. *)
(* One delayed request, one writer slot, and two generations expose stale  *)
(* admission, close/drain, quiescence, late callback, and storage reuse.    *)
(* Production binding: tracing/session_state.hpp and the native lifecycle   *)
(* plus concurrency specs exercise the packed generation/closed/count gate. *)
(* Does not prove SDK packet/filesystem behavior, Metal/driver behavior,   *)
(* GPU timestamps, rendering semantics, or final pixels.                   *)
(***************************************************************************)

CONSTANT Mode
Modes == {"Normal", "LateAdmission", "PrematureSeal", "AppendAfterSeal",
          "ReuseBeforeQuiescence"}
Phases == {"Idle", "Preparing", "Armed", "Collecting", "Closing",
           "Draining", "Finalizing", "Sealed"}

VARIABLES phase, generation, storageGeneration, requestGeneration,
          writerCount, writerGeneration, storageSealed, receipt,
          lateAdmission, sealWithWriter, appendAfterSeal, reuseWithWriter,
          staleWrite, staleAdmission, appendCount
vars == <<phase, generation, storageGeneration, requestGeneration,
          writerCount, writerGeneration, storageSealed, receipt,
          lateAdmission, sealWithWriter, appendAfterSeal, reuseWithWriter,
          staleWrite, staleAdmission, appendCount>>

Init ==
  /\ Mode \in Modes
  /\ phase = "Idle"
  /\ generation = 0
  /\ storageGeneration = 0
  /\ requestGeneration = 0
  /\ writerCount = 0
  /\ writerGeneration = 0
  /\ storageSealed = TRUE
  /\ receipt = FALSE
  /\ lateAdmission = FALSE
  /\ sealWithWriter = FALSE
  /\ appendAfterSeal = FALSE
  /\ reuseWithWriter = FALSE
  /\ staleWrite = FALSE
  /\ staleAdmission = FALSE
  /\ appendCount = 0

Start ==
  /\ phase = "Idle"
  /\ generation < 2
  /\ (writerCount = 0 \/ Mode = "ReuseBeforeQuiescence")
  /\ phase' = "Preparing"
  /\ generation' = generation + 1
  /\ storageGeneration' = generation + 1
  /\ writerCount' = writerCount
  /\ writerGeneration' = writerGeneration
  /\ storageSealed' = FALSE
  /\ receipt' = FALSE
  /\ reuseWithWriter' = (reuseWithWriter \/ (writerCount > 0))
  /\ UNCHANGED <<requestGeneration, lateAdmission, sealWithWriter,
                 appendAfterSeal, staleWrite, staleAdmission, appendCount>>

Ready ==
  /\ phase = "Preparing"
  /\ phase' = "Armed"
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

Trigger ==
  /\ phase = "Armed"
  /\ phase' = "Collecting"
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

ObserveRequest ==
  /\ phase = "Collecting"
  /\ requestGeneration' = generation
  /\ UNCHANGED <<phase, generation, storageGeneration, writerCount,
                 writerGeneration, storageSealed, receipt, lateAdmission,
                 sealWithWriter, appendAfterSeal, reuseWithWriter,
                 staleWrite, staleAdmission, appendCount>>

Admit ==
  /\ writerCount = 0
  /\ requestGeneration # 0
  /\ ((phase = "Collecting" /\ requestGeneration = generation) \/
      (Mode = "LateAdmission" /\ phase = "Draining" /\
       requestGeneration = generation) \/
      (Mode = "ReuseBeforeQuiescence"))
  /\ writerCount' = 1
  /\ writerGeneration' = requestGeneration
  /\ requestGeneration' = 0
  /\ lateAdmission' = (lateAdmission \/ (phase # "Collecting"))
  /\ staleAdmission' = (staleAdmission \/ (requestGeneration # generation))
  /\ UNCHANGED <<phase, generation, storageGeneration, storageSealed,
                 receipt, sealWithWriter, appendAfterSeal, reuseWithWriter,
                 staleWrite, appendCount>>

Close ==
  /\ phase = "Collecting"
  /\ phase' = "Closing"
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

Drain ==
  /\ phase = "Closing"
  /\ phase' = "Draining"
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

Append ==
  /\ writerCount = 1
  /\ appendCount < 2
  /\ (writerGeneration = storageGeneration \/
      Mode = "ReuseBeforeQuiescence")
  /\ (phase \in {"Collecting", "Closing", "Draining"} \/
      Mode \in {"AppendAfterSeal", "ReuseBeforeQuiescence"})
  /\ (~storageSealed \/ Mode = "AppendAfterSeal")
  /\ appendCount' = appendCount + 1
  /\ appendAfterSeal' = (appendAfterSeal \/ storageSealed)
  /\ staleWrite' = (staleWrite \/ (writerGeneration # storageGeneration))
  /\ UNCHANGED <<phase, generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, reuseWithWriter,
                 staleAdmission>>

WriterDone ==
  /\ writerCount = 1
  /\ writerCount' = 0
  /\ UNCHANGED <<phase, generation, storageGeneration, requestGeneration,
                 writerGeneration, storageSealed, receipt, lateAdmission,
                 sealWithWriter, appendAfterSeal, reuseWithWriter,
                 staleWrite, staleAdmission, appendCount>>

BeginFinalize ==
  /\ phase = "Draining"
  /\ (writerCount = 0 \/
      (Mode \in {"AppendAfterSeal", "ReuseBeforeQuiescence"} /\
       writerCount = 1))
  /\ phase' = "Finalizing"
  /\ storageSealed' = TRUE
  /\ sealWithWriter' = (sealWithWriter \/ (writerCount > 0))
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, receipt, lateAdmission,
                 appendAfterSeal, reuseWithWriter, staleWrite,
                 staleAdmission, appendCount>>

BadSeal ==
  /\ Mode = "PrematureSeal"
  /\ phase = "Draining"
  /\ writerCount = 1
  /\ phase' = "Finalizing"
  /\ storageSealed' = TRUE
  /\ sealWithWriter' = TRUE
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, receipt, lateAdmission,
                 appendAfterSeal, reuseWithWriter, staleWrite,
                 staleAdmission, appendCount>>

SerializeAndStop ==
  /\ phase = "Finalizing"
  /\ (writerCount = 0 \/
      (Mode = "ReuseBeforeQuiescence" /\ writerCount = 1))
  /\ phase' = "Sealed"
  /\ receipt' = TRUE
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

ReturnIdle ==
  /\ phase = "Sealed"
  /\ phase' = "Idle"
  /\ UNCHANGED <<generation, storageGeneration, requestGeneration,
                 writerCount, writerGeneration, storageSealed, receipt,
                 lateAdmission, sealWithWriter, appendAfterSeal,
                 reuseWithWriter, staleWrite, staleAdmission, appendCount>>

Next == Start \/ Ready \/ Trigger \/ ObserveRequest \/ Admit \/ Close \/ Drain
       \/ Append \/ WriterDone \/ BeginFinalize \/ BadSeal
       \/ SerializeAndStop \/ ReturnIdle
Spec == Init /\ [][Next]_vars
ProgressSpec == Spec /\ WF_vars(Ready) /\ WF_vars(Trigger)
               /\ WF_vars(ObserveRequest) /\ WF_vars(Admit)
               /\ WF_vars(Close) /\ WF_vars(Drain) /\ WF_vars(Append)
               /\ WF_vars(WriterDone) /\ WF_vars(BeginFinalize)
               /\ WF_vars(SerializeAndStop)

TypeOK ==
  /\ phase \in Phases
  /\ generation \in 0..2
  /\ storageGeneration \in 0..2
  /\ requestGeneration \in 0..2
  /\ writerCount \in 0..1
  /\ writerGeneration \in 0..2
  /\ storageSealed \in BOOLEAN
  /\ receipt \in BOOLEAN
  /\ lateAdmission \in BOOLEAN
  /\ sealWithWriter \in BOOLEAN
  /\ appendAfterSeal \in BOOLEAN
  /\ reuseWithWriter \in BOOLEAN
  /\ staleWrite \in BOOLEAN
  /\ staleAdmission \in BOOLEAN
  /\ appendCount \in 0..2

AdmittedOnlyDuringCollection == ~lateAdmission
SealWaitsForQuiescence == ~sealWithWriter
NoAppendToSealedStorage == ~appendAfterSeal
NoReuseWithLiveWriter == ~reuseWithWriter
NoStaleGenerationAppend == ~staleWrite
NoStaleGenerationAdmission == ~staleAdmission
WriterOwnsCurrentStorage == writerCount = 0 \/ writerGeneration = storageGeneration
SuccessfulReceiptIsSealed == receipt => phase \in {"Sealed", "Idle"}
CaptureEventuallySeals == phase = "Preparing" ~> phase = "Sealed"

=============================================================================
