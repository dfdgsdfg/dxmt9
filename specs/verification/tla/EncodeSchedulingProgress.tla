---- MODULE EncodeSchedulingProgress ----
(*
 * R-BACK-2.67 composed encode-scheduling progress model.
 *
 * Interface abstractions and their detailed owners:
 *   admission/publication/wake : CpuReadySessionProgress
 *   first capacity lease       : SessionCapacityLease
 *   progress generations       : ConcurrentProgressSignals
 *   FIFO session continuation  : EncodeSessionCompletion
 *   deferred payload retirement: PostEncodePayloadRetirement
 *   Present pacing             : PresentFrameLatency
 *
 * FirstLeaseWaitAction is the model-code truth-table binding for
 * classifyFirstLeaseCapacityWait. OrdinaryDirectSources denotes non-Present
 * Direct Arena Ready heads whose already-owned charge fits both ordinaryDirect
 * and highWater. The ExecuteOneSourceSerial action leaves openSession
 * unchanged, so pressure cannot create or enlarge a represented session or
 * its release event.
 *
 * Source arrival and GPU settlement are deliberately not queue actions.
 * Their assumptions are explicit below; all other fairness is limited to
 * queue-owned actions while they remain enabled.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxSources, MaxSessionLen, MaxPresentOutstanding,
          SeedDeniedFirstLeaseCycle, OrdinaryDirectSources

Sources == 1 .. MaxSources
Phases == {"Unaccepted", "Admission", "Leased", "Ready", "Encoded",
           "Submitted", "GpuSettled", "Completion", "TerminalDrain",
           "Released"}
Terminals == {"Running", "Stop", "DeviceLoss"}

VARIABLES
  phase,
  nextAccept,
  accepted,
  released,
  presentBearing,
  presentSubmitted,
  presentPublished,
  presentSkipped,
  presentSettled,
  payloadRetired,
  capacityOwner,
  capacityGeneration,
  capacityParked,
  capacityObservedGeneration,
  readyGeneration,
  encoderParked,
  encoderObservedGeneration,
  openSession,
  gpuQueue,
  gpuSettled,
  completionQueue,
  nextSessionId,
  sourceSession,
  terminal,
  firstLeaseParked,
  firstLeaseObservedGeneration,
  arenaAdmissionPressure,
  pressureSerialProgressAvailable,
  pressureEscaped

vars ==
  <<phase, nextAccept, accepted, released, presentBearing,
    presentSubmitted, presentPublished, presentSkipped, presentSettled,
    payloadRetired, capacityOwner, capacityGeneration, capacityParked,
    capacityObservedGeneration, readyGeneration, encoderParked,
    encoderObservedGeneration, openSession, gpuQueue, gpuSettled,
    completionQueue, nextSessionId, sourceSession, terminal,
    firstLeaseParked, firstLeaseObservedGeneration,
    arenaAdmissionPressure, pressureSerialProgressAvailable,
    pressureEscaped>>

SeqSet(seq) == {seq[i] : i \in DOMAIN seq}
AtPhase(name) == {s \in Sources : phase[s] = name}
Oldest(set) == CHOOSE s \in set : \A t \in set : s <= t
Increasing(seq) ==
  \A i \in DOMAIN seq : i < Len(seq) => seq[i] < seq[i + 1]

Init ==
  /\ phase = IF SeedDeniedFirstLeaseCycle
       THEN [s \in Sources |->
               IF s = 1 THEN "Submitted"
               ELSE IF s = 2 THEN "Ready"
               ELSE "Unaccepted"]
       ELSE [s \in Sources |-> "Unaccepted"]
  /\ nextAccept = IF SeedDeniedFirstLeaseCycle THEN 3 ELSE 1
  /\ accepted = IF SeedDeniedFirstLeaseCycle THEN {1, 2} ELSE {}
  /\ released = {}
  /\ presentBearing = IF SeedDeniedFirstLeaseCycle THEN {1} ELSE {}
  /\ presentSubmitted = IF SeedDeniedFirstLeaseCycle THEN {1} ELSE {}
  /\ presentPublished = IF SeedDeniedFirstLeaseCycle THEN {1} ELSE {}
  /\ presentSkipped = {}
  /\ presentSettled = {}
  /\ payloadRetired = {}
  /\ capacityOwner = 0
  /\ capacityGeneration = 0
  /\ capacityParked = 0
  /\ capacityObservedGeneration = 0
  /\ readyGeneration = 0
  /\ encoderParked = FALSE
  /\ encoderObservedGeneration = 0
  /\ openSession = <<>>
  /\ gpuQueue = IF SeedDeniedFirstLeaseCycle THEN <<1>> ELSE <<>>
  /\ gpuSettled = {}
  /\ completionQueue = <<>>
  /\ nextSessionId = IF SeedDeniedFirstLeaseCycle THEN 2 ELSE 1
  /\ sourceSession = IF SeedDeniedFirstLeaseCycle
       THEN [s \in Sources |-> IF s = 1 THEN 1 ELSE 0]
       ELSE [s \in Sources |-> 0]
  /\ terminal = "Running"
  /\ firstLeaseParked = IF SeedDeniedFirstLeaseCycle THEN 2 ELSE 0
  /\ firstLeaseObservedGeneration = capacityGeneration
  /\ arenaAdmissionPressure = FALSE
  /\ pressureSerialProgressAvailable = TRUE
  /\ pressureEscaped = {}

AcceptSource(isPresent) ==
  /\ terminal = "Running"
  /\ nextAccept \in Sources
  /\ LET s == nextAccept IN
       /\ phase' = [phase EXCEPT ![s] = "Admission"]
       /\ accepted' = accepted \cup {s}
       /\ presentBearing' =
            IF isPresent THEN presentBearing \cup {s} ELSE presentBearing
  /\ nextAccept' = nextAccept + 1
  /\ UNCHANGED <<released, presentSubmitted, presentPublished,
                  presentSkipped, presentSettled, payloadRetired,
                  capacityOwner, capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>

AcquireCapacity ==
  /\ terminal = "Running"
  /\ capacityOwner = 0
  /\ capacityParked = 0
  /\ AtPhase("Admission") # {}
  /\ LET s == Oldest(AtPhase("Admission")) IN
       /\ capacityOwner' = s
       /\ phase' = [phase EXCEPT ![s] = "Leased"]
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

ParkCapacityWaiter ==
  /\ terminal = "Running"
  /\ capacityOwner # 0
  /\ capacityParked = 0
  /\ AtPhase("Admission") # {}
  /\ capacityParked' = Oldest(AtPhase("Admission"))
  /\ capacityObservedGeneration' = capacityGeneration
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

WakeCapacityWaiter ==
  /\ capacityParked # 0
  /\ \/ terminal # "Running"
     \/ /\ capacityOwner = 0
        /\ capacityGeneration # capacityObservedGeneration
  /\ capacityParked' = 0
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

PublishSource ==
  /\ terminal = "Running"
  /\ capacityOwner # 0
  /\ LET s == capacityOwner IN
       /\ phase' = [phase EXCEPT ![s] = "Ready"]
  /\ capacityOwner' = 0
  /\ capacityGeneration' = capacityGeneration + 1
  /\ readyGeneration' = readyGeneration + 1
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityParked,
                  capacityObservedGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

ParkEncoder ==
  /\ terminal = "Running"
  /\ ~encoderParked
  /\ AtPhase("Ready") = {}
  /\ accepted \ released # {}
  /\ encoderParked' = TRUE
  /\ encoderObservedGeneration' = readyGeneration
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>

WakeEncoder ==
  /\ encoderParked
  /\ \/ terminal # "Running"
     \/ AtPhase("Ready") # {}
     \/ readyGeneration # encoderObservedGeneration
  /\ encoderParked' = FALSE
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

EncodeReadySource ==
  /\ terminal = "Running"
  /\ ~encoderParked
  /\ firstLeaseParked = 0
  /\ AtPhase("Ready") # {}
  /\ Len(openSession) < MaxSessionLen
  /\ LET s == Oldest(AtPhase("Ready")) IN
       /\ \/ s \notin presentBearing
          \/ Cardinality((presentSubmitted \ presentSettled) \cup
                         (SeqSet(openSession) \cap presentBearing))
                < MaxPresentOutstanding
       /\ phase' = [phase EXCEPT ![s] = "Encoded"]
       /\ openSession' = Append(openSession, s)
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

RetireDeferredPayload ==
  /\ terminal = "Running"
  /\ (AtPhase("Encoded") \ payloadRetired) \ presentBearing # {}
  /\ LET s == Oldest((AtPhase("Encoded") \ payloadRetired) \ presentBearing) IN
       payloadRetired' = payloadRetired \cup {s}
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

UndecidedPresents ==
  (AtPhase("Encoded") \ (presentPublished \cup presentSkipped))
      \cap presentBearing

PublishPresent ==
  /\ terminal = "Running"
  /\ UndecidedPresents # {}
  /\ LET s == Oldest(UndecidedPresents) IN
       presentPublished' = presentPublished \cup {s}
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentSkipped, presentSettled,
                  payloadRetired, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

SkipPresent ==
  /\ terminal = "Running"
  /\ UndecidedPresents # {}
  /\ LET s == Oldest(UndecidedPresents) IN
       presentSkipped' = presentSkipped \cup {s}
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSettled,
                  payloadRetired, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

SubmitSession ==
  /\ terminal = "Running"
  /\ openSession # <<>>
  /\ SeqSet(openSession) \cap presentBearing
         \subseteq presentPublished \cup presentSkipped
  /\ Cardinality((presentSubmitted \ presentSettled) \cup
                  (SeqSet(openSession) \cap presentBearing))
         <= MaxPresentOutstanding
  /\ phase' = [s \in Sources |->
       IF s \in SeqSet(openSession) THEN "Submitted" ELSE phase[s]]
  /\ gpuQueue' = gpuQueue \o openSession
  /\ presentSubmitted' = presentSubmitted \cup
       (SeqSet(openSession) \cap presentBearing)
  /\ sourceSession' = [s \in Sources |->
       IF s \in SeqSet(openSession) THEN nextSessionId ELSE sourceSession[s]]
  /\ nextSessionId' = nextSessionId + 1
  /\ openSession' = <<>>
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentPublished, presentSkipped, presentSettled,
                  payloadRetired, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, gpuSettled, completionQueue,
                  terminal>>

(* Explicit environment action; fairness appears only in
   GpuSettlementAssumption below. *)
GpuSettleHead ==
  /\ gpuQueue # <<>>
  /\ Head(gpuQueue) \notin gpuSettled
  (* The seeded older residency is deliberately unavailable: its settlement
     becomes reachable only after the queue makes the bounded serial step. *)
  /\ (~SeedDeniedFirstLeaseCycle \/ pressureEscaped # {} \/
        terminal # "Running")
  /\ LET s == Head(gpuQueue) IN
       /\ gpuSettled' = gpuSettled \cup {s}
       /\ phase' = [phase EXCEPT ![s] = "GpuSettled"]
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, completionQueue, nextSessionId, sourceSession,
                  terminal>>

ExpandCompletion ==
  /\ gpuQueue # <<>>
  /\ Head(gpuQueue) \in gpuSettled
  /\ LET s == Head(gpuQueue) IN
       /\ phase' = [phase EXCEPT ![s] = "Completion"]
       /\ gpuQueue' = Tail(gpuQueue)
       /\ completionQueue' = Append(completionQueue, s)
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuSettled, nextSessionId, sourceSession, terminal>>

ReleaseSource ==
  /\ completionQueue # <<>>
  /\ Head(completionQueue) = Cardinality(released) + 1
  /\ LET s == Head(completionQueue) IN
       /\ phase' = [phase EXCEPT ![s] = "Released"]
       /\ released' = released \cup {s}
       /\ payloadRetired' = payloadRetired \cup {s}
       /\ completionQueue' = Tail(completionQueue)
       /\ capacityGeneration' = capacityGeneration + 1
  /\ UNCHANGED <<nextAccept, accepted, presentBearing, presentSubmitted,
                  presentPublished, presentSkipped, presentSettled,
                  capacityOwner, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, nextSessionId, sourceSession,
                  terminal>>

SettlePresent ==
  /\ (released \cap presentBearing) \ presentSettled # {}
  /\ LET s == Oldest((released \cap presentBearing) \ presentSettled) IN
       presentSettled' = presentSettled \cup {s}
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  payloadRetired, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

RequestTerminal(disposition) ==
  /\ terminal = "Running"
  /\ nextAccept = MaxSources + 1
  /\ disposition \in {"Stop", "DeviceLoss"}
  /\ terminal' = disposition
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession>>

(* Terminal teardown is stage-specific. No transition below fabricates a
   submission, GPU settlement, Present decision, and source release at once. *)
TerminalDrainSource(stage) ==
  /\ terminal # "Running"
  /\ stage \in {"Admission", "Leased", "Ready", "Encoded"}
  /\ AtPhase(stage) # {}
  /\ LET s == Oldest(AtPhase(stage)) IN
       /\ s # firstLeaseParked
       /\ phase' = [phase EXCEPT ![s] = "TerminalDrain"]
       /\ openSession' = SelectSeq(openSession, LAMBDA x : x # s)
       /\ capacityOwner' = IF capacityOwner = s THEN 0 ELSE capacityOwner
       /\ capacityGeneration' =
            IF capacityOwner = s THEN capacityGeneration + 1
            ELSE capacityGeneration
       /\ capacityParked' = IF capacityParked = s THEN 0 ELSE capacityParked
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

TerminalDrainAdmission == TerminalDrainSource("Admission")
TerminalDrainLeased == TerminalDrainSource("Leased")
TerminalDrainReady == TerminalDrainSource("Ready")
TerminalDrainEncoded == TerminalDrainSource("Encoded")

TerminalSkipPresent ==
  /\ terminal # "Running"
  /\ (AtPhase("TerminalDrain") \cap presentBearing) \
       (presentPublished \cup presentSkipped) # {}
  /\ LET s == Oldest((AtPhase("TerminalDrain") \cap presentBearing) \
                      (presentPublished \cup presentSkipped)) IN
       presentSkipped' = presentSkipped \cup {s}
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSettled,
                  payloadRetired, capacityOwner, capacityGeneration,
                  capacityParked, capacityObservedGeneration,
                  readyGeneration, encoderParked,
                  encoderObservedGeneration, openSession, gpuQueue,
                  gpuSettled, completionQueue, nextSessionId, sourceSession,
                  terminal>>

TerminalReleaseDrained ==
  /\ terminal # "Running"
  /\ AtPhase("TerminalDrain") # {}
  /\ LET s == Oldest(AtPhase("TerminalDrain")) IN
       /\ s = Cardinality(released) + 1
       /\ (s \notin presentBearing \/
            s \in presentPublished \cup presentSkipped)
       /\ phase' = [phase EXCEPT ![s] = "Released"]
       /\ released' = released \cup {s}
       /\ payloadRetired' = payloadRetired \cup {s}
  /\ UNCHANGED <<nextAccept, accepted, presentBearing, presentSubmitted,
                  presentPublished, presentSkipped, presentSettled,
                  capacityOwner, capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>

FirstLeaseWaitAction ==
  IF terminal # "Running" THEN "Stop"
  ELSE IF capacityGeneration # firstLeaseObservedGeneration THEN "RetryLease"
  ELSE IF arenaAdmissionPressure /\ pressureSerialProgressAvailable /\
          firstLeaseParked \in OrdinaryDirectSources /\
          firstLeaseParked \in AtPhase("Ready")
       THEN "ExecuteOneSourceSerial"
  ELSE "Wait"

BeginArenaAdmissionPressure ==
  /\ terminal = "Running"
  /\ firstLeaseParked # 0
  /\ ~arenaAdmissionPressure
  /\ arenaAdmissionPressure' = TRUE
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>
  /\ firstLeaseParked' = firstLeaseParked
  /\ firstLeaseObservedGeneration' = firstLeaseObservedGeneration
  /\ pressureSerialProgressAvailable' = pressureSerialProgressAvailable
  /\ pressureEscaped' = pressureEscaped

PressureSerialEscape ==
  /\ FirstLeaseWaitAction = "ExecuteOneSourceSerial"
  /\ LET s == firstLeaseParked IN
       /\ s = Oldest(AtPhase("Ready"))
       /\ s \notin presentBearing
       /\ phase' = [phase EXCEPT ![s] = "Submitted"]
       /\ gpuQueue' = Append(gpuQueue, s)
       /\ sourceSession' = [sourceSession EXCEPT ![s] = nextSessionId]
       /\ nextSessionId' = nextSessionId + 1
       /\ pressureEscaped' = pressureEscaped \cup {s}
  /\ firstLeaseParked' = 0
  /\ pressureSerialProgressAvailable' = FALSE
  /\ UNCHANGED <<nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuSettled, completionQueue, terminal,
                  firstLeaseObservedGeneration, arenaAdmissionPressure>>

WakeFirstLeaseWaiter ==
  /\ firstLeaseParked # 0
  /\ FirstLeaseWaitAction \in {"RetryLease", "Stop"}
  /\ firstLeaseParked' = 0
  /\ pressureSerialProgressAvailable' =
       IF FirstLeaseWaitAction = "RetryLease" THEN TRUE
       ELSE pressureSerialProgressAvailable
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>
  /\ firstLeaseObservedGeneration' = firstLeaseObservedGeneration
  /\ arenaAdmissionPressure' = arenaAdmissionPressure
  /\ pressureEscaped' = pressureEscaped

ClearArenaAdmissionPressure ==
  /\ arenaAdmissionPressure
  /\ \/ terminal # "Running"
     \/ capacityGeneration # firstLeaseObservedGeneration
  /\ arenaAdmissionPressure' = FALSE
  /\ UNCHANGED <<phase, nextAccept, accepted, released, presentBearing,
                  presentSubmitted, presentPublished, presentSkipped,
                  presentSettled, payloadRetired, capacityOwner,
                  capacityGeneration, capacityParked,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, openSession,
                  gpuQueue, gpuSettled, completionQueue, nextSessionId,
                  sourceSession, terminal>>
  /\ firstLeaseParked' = firstLeaseParked
  /\ firstLeaseObservedGeneration' = firstLeaseObservedGeneration
  /\ pressureSerialProgressAvailable' = pressureSerialProgressAvailable
  /\ pressureEscaped' = pressureEscaped

AcceptNext == AcceptSource(nextAccept = MaxSources)

BaseNext ==
  \/ AcceptNext
  \/ AcquireCapacity
  \/ ParkCapacityWaiter
  \/ WakeCapacityWaiter
  \/ PublishSource
  \/ ParkEncoder
  \/ WakeEncoder
  \/ EncodeReadySource
  \/ RetireDeferredPayload
  \/ PublishPresent
  \/ SkipPresent
  \/ SubmitSession
  \/ GpuSettleHead
  \/ ExpandCompletion
  \/ ReleaseSource
  \/ SettlePresent
  \/ RequestTerminal("Stop")
  \/ RequestTerminal("DeviceLoss")
  \/ TerminalDrainAdmission
  \/ TerminalDrainLeased
  \/ TerminalDrainReady
  \/ TerminalDrainEncoded
  \/ TerminalSkipPresent
  \/ TerminalReleaseDrained

PreserveFirstLease ==
  /\ firstLeaseParked' = firstLeaseParked
  /\ firstLeaseObservedGeneration' = firstLeaseObservedGeneration
  /\ arenaAdmissionPressure' = arenaAdmissionPressure
  /\ pressureSerialProgressAvailable' = pressureSerialProgressAvailable
  /\ pressureEscaped' = pressureEscaped

Next ==
  \/ BaseNext /\ PreserveFirstLease
  \/ BeginArenaAdmissionPressure
  \/ PressureSerialEscape
  \/ WakeFirstLeaseWaiter
  \/ ClearArenaAdmissionPressure

QueueFairness ==
  /\ WF_vars(AcquireCapacity /\ PreserveFirstLease)
  /\ WF_vars(WakeCapacityWaiter /\ PreserveFirstLease)
  /\ WF_vars(PublishSource /\ PreserveFirstLease)
  /\ WF_vars(WakeEncoder /\ PreserveFirstLease)
  /\ WF_vars(EncodeReadySource /\ PreserveFirstLease)
  /\ WF_vars(PublishPresent /\ PreserveFirstLease)
  /\ WF_vars(SkipPresent /\ PreserveFirstLease)
  /\ WF_vars(SubmitSession /\ PreserveFirstLease)
  /\ WF_vars(ExpandCompletion /\ PreserveFirstLease)
  /\ WF_vars(ReleaseSource /\ PreserveFirstLease)
  /\ WF_vars(SettlePresent /\ PreserveFirstLease)
  /\ WF_vars(TerminalDrainAdmission /\ PreserveFirstLease)
  /\ WF_vars(TerminalDrainLeased /\ PreserveFirstLease)
  /\ WF_vars(TerminalDrainReady /\ PreserveFirstLease)
  /\ WF_vars(TerminalDrainEncoded /\ PreserveFirstLease)
  /\ WF_vars(TerminalSkipPresent /\ PreserveFirstLease)
  /\ WF_vars(TerminalReleaseDrained /\ PreserveFirstLease)
  /\ WF_vars(BeginArenaAdmissionPressure)
  /\ WF_vars(PressureSerialEscape)
  /\ WF_vars(WakeFirstLeaseWaiter)
  /\ WF_vars(ClearArenaAdmissionPressure)

GpuSettlementAssumption == WF_vars(GpuSettleHead /\ PreserveFirstLease)
SourceArrivalAssumption == WF_vars(AcceptNext /\ PreserveFirstLease)

Spec ==
  Init /\ [][Next]_vars /\ QueueFairness /\ GpuSettlementAssumption /\
    SourceArrivalAssumption

TypeOK ==
  /\ MaxSources \in Nat \ {0}
  /\ MaxSessionLen \in Nat \ {0}
  /\ MaxPresentOutstanding \in Nat \ {0}
  /\ SeedDeniedFirstLeaseCycle \in BOOLEAN
  /\ SeedDeniedFirstLeaseCycle => MaxSources >= 2
  /\ OrdinaryDirectSources \subseteq Sources
  /\ phase \in [Sources -> Phases]
  /\ nextAccept \in 1 .. (MaxSources + 1)
  /\ accepted \subseteq Sources
  /\ released \subseteq accepted
  /\ presentBearing \subseteq accepted
  /\ presentSubmitted \subseteq presentBearing
  /\ presentPublished \subseteq presentBearing
  /\ presentSkipped \subseteq presentBearing
  /\ presentSettled \subseteq presentBearing
  /\ payloadRetired \subseteq accepted
  /\ capacityOwner \in 0 .. MaxSources
  /\ capacityGeneration \in Nat
  /\ capacityParked \in 0 .. MaxSources
  /\ capacityObservedGeneration \in Nat
  /\ readyGeneration \in Nat
  /\ encoderParked \in BOOLEAN
  /\ encoderObservedGeneration \in Nat
  /\ openSession \in Seq(Sources)
  /\ gpuQueue \in Seq(Sources)
  /\ gpuSettled \subseteq Sources
  /\ completionQueue \in Seq(Sources)
  /\ nextSessionId \in Nat \ {0}
  /\ sourceSession \in [Sources -> Nat]
  /\ terminal \in Terminals
  /\ firstLeaseParked \in 0 .. MaxSources
  /\ firstLeaseObservedGeneration \in Nat
  /\ arenaAdmissionPressure \in BOOLEAN
  /\ pressureSerialProgressAvailable \in BOOLEAN
  /\ pressureEscaped \subseteq Sources

BoundedStores ==
  /\ Len(openSession) <= MaxSessionLen
  /\ Len(gpuQueue) <= MaxSources
  /\ Len(completionQueue) <= MaxSources

OwnershipConservation ==
  /\ accepted = {s \in Sources : phase[s] # "Unaccepted"}
  /\ released = AtPhase("Released")
  /\ Cardinality(accepted) =
       Cardinality(AtPhase("Admission")) +
       Cardinality(AtPhase("Leased")) +
       Cardinality(AtPhase("Ready")) +
       Cardinality(AtPhase("Encoded")) +
       Cardinality(AtPhase("Submitted")) +
       Cardinality(AtPhase("GpuSettled")) +
       Cardinality(AtPhase("Completion")) +
       Cardinality(AtPhase("TerminalDrain")) +
       Cardinality(AtPhase("Released"))
  /\ SeqSet(openSession) = AtPhase("Encoded")
  /\ SeqSet(gpuQueue) = AtPhase("Submitted") \cup AtPhase("GpuSettled")
  /\ SeqSet(completionQueue) = AtPhase("Completion")
  /\ (capacityOwner = 0 \/ phase[capacityOwner] = "Leased")
  /\ (capacityParked = 0 \/ phase[capacityParked] = "Admission")
  /\ (firstLeaseParked = 0 \/ phase[firstLeaseParked] = "Ready")

FifoSessionAndCompletion ==
  /\ Increasing(openSession)
  /\ Increasing(gpuQueue)
  /\ Increasing(completionQueue)
  /\ released = 1 .. Cardinality(released)
  /\ \A s \in accepted : sourceSession[s] # 0 =>
       \A t \in accepted :
         sourceSession[t] = sourceSession[s] /\ s < t =>
           phase[s] # "Unaccepted"

PayloadRetirementSafety ==
  /\ payloadRetired \cap presentBearing \subseteq released
  /\ released \subseteq payloadRetired

PresentDecisionSeparation ==
  /\ presentPublished \cap presentSkipped = {}
  /\ presentSettled \subseteq presentPublished \cup presentSkipped
  /\ Cardinality(presentSubmitted \ presentSettled) <=
       MaxPresentOutstanding

StickyObligations ==
  /\ released \subseteq accepted
  /\ presentPublished \cup presentSkipped \cup presentSettled
       \subseteq presentBearing

CapacityLostWakeupFreedom ==
  capacityParked # 0 /\
      (terminal # "Running" \/
       (capacityOwner = 0 /\
        capacityGeneration # capacityObservedGeneration))
    => ENABLED WakeCapacityWaiter

EncoderLostWakeupFreedom ==
  encoderParked /\
      (terminal # "Running" \/ AtPhase("Ready") # {} \/
       readyGeneration # encoderObservedGeneration)
    => ENABLED WakeEncoder

FirstLeaseWaitActionEnabled ==
  firstLeaseParked # 0 /\ FirstLeaseWaitAction # "Wait" =>
    \/ FirstLeaseWaitAction = "ExecuteOneSourceSerial" /\
         ENABLED PressureSerialEscape
    \/ FirstLeaseWaitAction \in {"RetryLease", "Stop"} /\
         ENABLED WakeFirstLeaseWaiter

PressureEscapeIsStandalone ==
  /\ pressureEscaped \cap SeqSet(openSession) = {}
  /\ \A s \in pressureEscaped :
       /\ sourceSession[s] # 0
       /\ \A t \in accepted \ {s} :
            sourceSession[t] # sourceSession[s]

TerminalCapacityWaiterUnblocked ==
  terminal # "Running" ~> capacityParked = 0

TerminalEncoderWaiterUnblocked ==
  terminal # "Running" ~> ~encoderParked

StickyTrackingStep ==
  /\ accepted \subseteq accepted'
  /\ released \subseteq released'
  /\ presentBearing \subseteq presentBearing'
  /\ presentPublished \subseteq presentPublished'
  /\ presentSkipped \subseteq presentSkipped'
  /\ presentSettled \subseteq presentSettled'
  /\ pressureEscaped \subseteq pressureEscaped'

StickyTracking == [][StickyTrackingStep]_vars

EveryAcceptedSourceReleased ==
  \A s \in Sources : s \in accepted ~> s \in released

EveryPresentDecided ==
  \A s \in Sources : s \in presentBearing ~>
    s \in presentPublished \cup presentSkipped

EveryPresentSettled ==
  \A s \in Sources : s \in presentBearing ~> s \in presentSettled

SeededDeniedLeasePressureProgress ==
  SeedDeniedFirstLeaseCycle =>
    ([] (terminal = "Running") =>
      /\ <>arenaAdmissionPressure
      /\ <> (2 \in pressureEscaped))

EveryPressureEscapedSourceReleased ==
  \A s \in Sources : s \in pressureEscaped ~> s \in released

DeniedFirstLeaseAdmissionCycleBroken ==
  \A s \in OrdinaryDirectSources :
    [] (terminal = "Running") =>
      (firstLeaseParked = s /\ arenaAdmissionPressure /\
          pressureSerialProgressAvailable
        ~> s \in pressureEscaped)

====
