----------------------------- MODULE TracingGpuRetirement -----------------------------
EXTENDS Naturals, TLC

(***************************************************************************)
(* One diagnostic slot, two sessions, and three slot generations model the  *)
(* Collector publication/complete/drain protocol in dxmt9_trace_gpu.cpp.    *)
(* A retained duplicate completion exposes the old Free->Reserved bug.     *)
(* GPU completion is an external fact; close/timeout only quarantine.      *)
(* The reservation, callback, and consumer leases abstract bridge CPU      *)
(* users. This does not prove Metal timestamps, API synchronization, pixel *)
(* correctness, raw impl_ pointer admission, or unbounded generation wrap. *)
(* R-HARN-TRACE-10.3/10.4; R-VERIF-1.1 through R-VERIF-1.4. Fair GPU       *)
(* completion is a progress premise, never a profiling-induced wait.       *)
(***************************************************************************)

CONSTANT Mode
Modes == {"Normal", "EarlyPublication", "StaleTicket", "EarlyReuse",
          "SessionReuse", "EarlyRetire", "ReaderRetire"}
SlotStates == {"Free", "Initializing", "Reserved", "Completing", "Ready", "Draining"}

VARIABLES session, open, state, slotGeneration, payloadReady,
          reservationLive, callbackLive, readerLive, gpuPending, gpuDone,
          delayedSession, delayedSlot, quarantined, retired,
          badPublication, badTicket, badReuse, badSessionReuse
vars == <<session, open, state, slotGeneration, payloadReady,
          reservationLive, callbackLive, readerLive, gpuPending, gpuDone,
          delayedSession, delayedSlot, quarantined, retired,
          badPublication, badTicket, badReuse, badSessionReuse>>

Init ==
  /\ Mode \in Modes
  /\ session = 0
  /\ open = FALSE
  /\ state = "Free"
  /\ slotGeneration = 0
  /\ payloadReady = FALSE
  /\ reservationLive = FALSE
  /\ callbackLive = FALSE
  /\ readerLive = FALSE
  /\ gpuPending = FALSE
  /\ gpuDone = FALSE
  /\ delayedSession = 0
  /\ delayedSlot = 0
  /\ quarantined = FALSE
  /\ retired = FALSE
  /\ badPublication = FALSE
  /\ badTicket = FALSE
  /\ badReuse = FALSE
  /\ badSessionReuse = FALSE

NoUsers == ~reservationLive /\ ~callbackLive /\ ~readerLive
CanBeginSession == ~open /\ NoUsers /\ state = "Free" /\ ~gpuPending
TicketMatches(s, g) == s # 0 /\ g # 0 /\ s = session /\ g = slotGeneration
CanRetireStorage == ~open /\ NoUsers /\ state = "Free" /\ ~gpuPending

Start ==
  /\ ~retired /\ ~quarantined
  /\ session < 2
  /\ (CanBeginSession \/ (Mode = "SessionReuse" /\ ~open))
  /\ session' = session + 1
  /\ open' = TRUE
  /\ state' = "Free"
  /\ payloadReady' = FALSE
  /\ quarantined' = FALSE
  /\ badSessionReuse' = (badSessionReuse \/ ~CanBeginSession)
  /\ UNCHANGED <<slotGeneration, reservationLive, callbackLive, readerLive,
                 gpuPending, gpuDone, delayedSession, delayedSlot, retired,
                 badPublication, badTicket, badReuse>>

Reserve ==
  /\ ~retired /\ open /\ state = "Free"
  /\ slotGeneration < 3
  /\ ~reservationLive
  /\ state' = IF Mode = "EarlyPublication" THEN "Reserved" ELSE "Initializing"
  /\ reservationLive' = TRUE
  /\ payloadReady' = FALSE
  /\ gpuDone' = FALSE
  /\ badReuse' = (badReuse \/ readerLive \/ callbackLive \/ gpuPending)
  /\ UNCHANGED <<session, open, slotGeneration, callbackLive, readerLive,
                 gpuPending, delayedSession, delayedSlot, quarantined, retired,
                 badPublication, badTicket, badSessionReuse>>

InitializePayload ==
  /\ ~retired /\ reservationLive /\ ~payloadReady
  /\ state \in {"Initializing", "Reserved"}
  /\ slotGeneration' = slotGeneration + 1
  /\ payloadReady' = TRUE
  /\ UNCHANGED <<session, open, state, reservationLive, callbackLive,
                 readerLive, gpuPending, gpuDone, delayedSession, delayedSlot,
                 quarantined, retired, badPublication, badTicket, badReuse,
                 badSessionReuse>>

Publish ==
  /\ ~retired /\ reservationLive /\ payloadReady
  /\ state \in {"Initializing", "Reserved"}
  /\ state' = "Reserved"
  /\ reservationLive' = FALSE
  /\ gpuPending' = TRUE
  /\ UNCHANGED <<session, open, slotGeneration, payloadReady, callbackLive,
                 readerLive, gpuDone, delayedSession, delayedSlot, quarantined,
                 retired, badPublication, badTicket, badReuse, badSessionReuse>>

GpuCompletes ==
  /\ ~retired /\ state = "Reserved" /\ gpuPending
  /\ gpuPending' = FALSE
  /\ gpuDone' = TRUE
  /\ UNCHANGED <<session, open, state, slotGeneration, payloadReady,
                 reservationLive, callbackLive, readerLive, delayedSession,
                 delayedSlot, quarantined, retired, badPublication, badTicket,
                 badReuse, badSessionReuse>>

ClaimCurrent ==
  /\ ~retired /\ state = "Reserved" /\ gpuDone
  /\ ~reservationLive /\ ~callbackLive /\ payloadReady
  /\ state' = "Completing"
  /\ callbackLive' = TRUE
  /\ UNCHANGED <<session, open, slotGeneration, payloadReady,
                 reservationLive, readerLive, gpuPending, gpuDone,
                 delayedSession, delayedSlot, quarantined, retired,
                 badPublication, badTicket, badReuse, badSessionReuse>>

ClaimDelayed ==
  /\ ~retired /\ state = "Reserved" /\ ~callbackLive
  /\ delayedSession # 0 /\ delayedSlot # 0
  /\ (TicketMatches(delayedSession, delayedSlot) \/ Mode = "StaleTicket")
  /\ state' = "Completing"
  /\ callbackLive' = TRUE
  /\ badPublication' = (badPublication \/ reservationLive \/ ~payloadReady)
  /\ badTicket' = (badTicket \/ ~TicketMatches(delayedSession, delayedSlot))
  /\ UNCHANGED <<session, open, slotGeneration, payloadReady, reservationLive,
                 readerLive, gpuPending, gpuDone, delayedSession, delayedSlot,
                 quarantined, retired, badReuse, badSessionReuse>>

PublishCompletion ==
  /\ ~retired /\ state = "Completing" /\ callbackLive
  /\ state' = "Ready"
  /\ callbackLive' = FALSE
  /\ delayedSession' = session
  /\ delayedSlot' = slotGeneration
  /\ UNCHANGED <<session, open, slotGeneration, payloadReady, reservationLive,
                 readerLive, gpuPending, gpuDone, quarantined, retired,
                 badPublication, badTicket, badReuse, badSessionReuse>>

BeginDrain ==
  /\ ~retired /\ state = "Ready" /\ ~readerLive
  /\ state' = IF Mode = "EarlyReuse" THEN "Free" ELSE "Draining"
  /\ readerLive' = TRUE
  /\ UNCHANGED <<session, open, slotGeneration, payloadReady, reservationLive,
                 callbackLive, gpuPending, gpuDone, delayedSession, delayedSlot,
                 quarantined, retired, badPublication, badTicket, badReuse,
                 badSessionReuse>>

EndDrain ==
  /\ ~retired /\ readerLive
  /\ state = "Draining"
  /\ state' = "Free"
  /\ readerLive' = FALSE
  /\ payloadReady' = FALSE
  /\ UNCHANGED <<session, open, slotGeneration, reservationLive, callbackLive,
                 gpuPending, gpuDone, delayedSession, delayedSlot, quarantined,
                 retired, badPublication, badTicket, badReuse, badSessionReuse>>

Close ==
  /\ ~retired /\ open
  /\ open' = FALSE
  /\ UNCHANGED <<session, state, slotGeneration, payloadReady, reservationLive,
                 callbackLive, readerLive, gpuPending, gpuDone, delayedSession,
                 delayedSlot, quarantined, retired, badPublication, badTicket,
                 badReuse, badSessionReuse>>

Quarantine ==
  /\ ~retired /\ ~open /\ ~CanRetireStorage /\ ~quarantined
  /\ quarantined' = TRUE
  /\ UNCHANGED <<session, open, state, slotGeneration, payloadReady,
                 reservationLive, callbackLive, readerLive, gpuPending, gpuDone,
                 delayedSession, delayedSlot, retired, badPublication, badTicket,
                 badReuse, badSessionReuse>>

Retire ==
  /\ ~retired /\ session # 0 /\ ~open
  /\ (CanRetireStorage \/
      (Mode = "EarlyRetire" /\ gpuPending) \/
      (Mode = "ReaderRetire" /\ readerLive))
  /\ retired' = TRUE
  /\ UNCHANGED <<session, open, state, slotGeneration, payloadReady,
                 reservationLive, callbackLive, readerLive, gpuPending, gpuDone,
                 delayedSession, delayedSlot, quarantined, badPublication,
                 badTicket, badReuse, badSessionReuse>>

Next == Start \/ Reserve \/ InitializePayload \/ Publish \/ GpuCompletes
        \/ ClaimCurrent \/ ClaimDelayed \/ PublishCompletion \/ BeginDrain
        \/ EndDrain \/ Close \/ Quarantine \/ Retire
Spec == Init /\ [][Next]_vars
ProgressSpec == Spec /\ WF_vars(InitializePayload) /\ WF_vars(Publish)
                /\ WF_vars(GpuCompletes) /\ WF_vars(ClaimCurrent)
                /\ WF_vars(PublishCompletion) /\ WF_vars(BeginDrain)
                /\ WF_vars(EndDrain) /\ WF_vars(Retire)

TypeOK ==
  /\ session \in 0..2
  /\ slotGeneration \in 0..3
  /\ delayedSession \in 0..2
  /\ delayedSlot \in 0..3
  /\ state \in SlotStates
  /\ \A value \in {open, payloadReady, reservationLive, callbackLive,
                   readerLive, gpuPending, gpuDone, quarantined, retired,
                   badPublication, badTicket, badReuse, badSessionReuse}:
       value \in BOOLEAN
NoCompletionBeforePublication == ~badPublication
NoStaleCompletion == ~badTicket
NoReuseBeforeConsumption == ~badReuse
SessionReuseRequiresQuiescence == ~badSessionReuse
NoRetireBeforeGpuCompletion == retired => ~gpuPending
NoRetireWithCpuReaders == retired => NoUsers
RetirementRequiresConsumedResult == retired => state = "Free"
QuarantineRetainsLiveStorage == quarantined /\ ~CanRetireStorage => ~retired
GpuEventuallyConsumed == gpuPending ~> state = "Free"
QuarantineEventuallyRetires == quarantined ~> retired

=============================================================================
