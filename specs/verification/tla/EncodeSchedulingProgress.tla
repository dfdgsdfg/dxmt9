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
 * GPU settlement is deliberately not a queue action. GpuSettlementAssumption
 * is the explicit environment fairness assumption; all other fairness below
 * is limited to queue-owned actions while they remain enabled.
 *)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS MaxSources, MaxSessionLen, MaxPresentOutstanding

Sources == 1 .. MaxSources
Phases == {"Unaccepted", "Admission", "Leased", "Ready", "Encoded",
           "Submitted", "GpuSettled", "Completion", "Released"}
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
  terminal

vars == <<phase, nextAccept, accepted, released, presentBearing,
          presentSubmitted, presentPublished, presentSkipped, presentSettled,
          payloadRetired, capacityOwner, capacityGeneration, capacityParked,
          capacityObservedGeneration, readyGeneration, encoderParked,
          encoderObservedGeneration, openSession, gpuQueue, gpuSettled,
          completionQueue, nextSessionId, sourceSession, terminal>>

SeqSet(seq) == {seq[i] : i \in DOMAIN seq}
AtPhase(name) == {s \in Sources : phase[s] = name}
Oldest(set) == CHOOSE s \in set : \A t \in set : s <= t
Increasing(seq) ==
  \A i \in DOMAIN seq : i < Len(seq) => seq[i] < seq[i + 1]

Init ==
  /\ phase = [s \in Sources |-> "Unaccepted"]
  /\ nextAccept = 1
  /\ accepted = {}
  /\ released = {}
  /\ presentBearing = {}
  /\ presentSubmitted = {}
  /\ presentPublished = {}
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
  /\ gpuQueue = <<>>
  /\ gpuSettled = {}
  /\ completionQueue = <<>>
  /\ nextSessionId = 1
  /\ sourceSession = [s \in Sources |-> 0]
  /\ terminal = "Running"

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
  /\ terminal = "Running"
  /\ gpuQueue # <<>>
  /\ Head(gpuQueue) \notin gpuSettled
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
  /\ terminal = "Running"
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
  /\ terminal = "Running"
  /\ completionQueue # <<>>
  /\ Head(completionQueue) = Cardinality(released) + 1
  /\ LET s == Head(completionQueue) IN
       /\ phase' = [phase EXCEPT ![s] = "Released"]
       /\ released' = released \cup {s}
       /\ payloadRetired' = payloadRetired \cup {s}
       /\ completionQueue' = Tail(completionQueue)
  /\ UNCHANGED <<nextAccept, accepted, presentBearing, presentSubmitted,
                  presentPublished, presentSkipped, presentSettled,
                  capacityOwner, capacityGeneration, capacityParked,
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

TerminalRelease ==
  /\ terminal # "Running"
  /\ accepted \ released # {}
  /\ LET s == Oldest(accepted \ released) IN
       /\ phase' = [phase EXCEPT ![s] = "Released"]
       /\ released' = released \cup {s}
       /\ payloadRetired' = payloadRetired \cup {s}
       /\ presentSubmitted' =
            IF s \in presentBearing THEN presentSubmitted \cup {s}
            ELSE presentSubmitted
       /\ presentSkipped' =
            IF s \in (presentBearing \ presentPublished)
            THEN presentSkipped \cup {s} ELSE presentSkipped
       /\ presentSettled' =
            IF s \in presentBearing THEN presentSettled \cup {s}
            ELSE presentSettled
       /\ openSession' = SelectSeq(openSession, LAMBDA x : x # s)
       /\ gpuQueue' = SelectSeq(gpuQueue, LAMBDA x : x # s)
       /\ completionQueue' = SelectSeq(completionQueue, LAMBDA x : x # s)
       /\ capacityOwner' = IF capacityOwner = s THEN 0 ELSE capacityOwner
       /\ capacityGeneration' =
            IF capacityOwner = s THEN capacityGeneration + 1
            ELSE capacityGeneration
       /\ capacityParked' = IF capacityParked = s THEN 0 ELSE capacityParked
  /\ UNCHANGED <<nextAccept, accepted, presentBearing, presentPublished,
                  capacityObservedGeneration, readyGeneration,
                  encoderParked, encoderObservedGeneration, gpuSettled,
                  nextSessionId, sourceSession, terminal>>

Next ==
  \/ AcceptSource(FALSE)
  \/ AcceptSource(TRUE)
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
  \/ TerminalRelease

QueueFairness ==
  /\ WF_vars(AcquireCapacity)
  /\ WF_vars(WakeCapacityWaiter)
  /\ WF_vars(PublishSource)
  /\ WF_vars(WakeEncoder)
  /\ WF_vars(EncodeReadySource)
  /\ WF_vars(PublishPresent)
  /\ WF_vars(SkipPresent)
  /\ WF_vars(SubmitSession)
  /\ WF_vars(ExpandCompletion)
  /\ WF_vars(ReleaseSource)
  /\ WF_vars(SettlePresent)
  /\ WF_vars(TerminalRelease)

GpuSettlementAssumption == WF_vars(GpuSettleHead)

Spec ==
  Init /\ [][Next]_vars /\ QueueFairness /\ GpuSettlementAssumption

TypeOK ==
  /\ MaxSources \in Nat \ {0}
  /\ MaxSessionLen \in Nat \ {0}
  /\ MaxPresentOutstanding \in Nat \ {0}
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
       Cardinality(AtPhase("Released"))
  /\ SeqSet(openSession) = AtPhase("Encoded")
  /\ SeqSet(gpuQueue) = AtPhase("Submitted") \cup AtPhase("GpuSettled")
  /\ SeqSet(completionQueue) = AtPhase("Completion")
  /\ (capacityOwner = 0 \/ phase[capacityOwner] = "Leased")
  /\ (capacityParked = 0 \/ phase[capacityParked] = "Admission")

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

StickyTrackingStep ==
  /\ accepted \subseteq accepted'
  /\ released \subseteq released'
  /\ presentBearing \subseteq presentBearing'
  /\ presentPublished \subseteq presentPublished'
  /\ presentSkipped \subseteq presentSkipped'
  /\ presentSettled \subseteq presentSettled'

StickyTracking == [][StickyTrackingStep]_vars

EveryAcceptedSourceReleased ==
  \A s \in Sources : s \in accepted ~> s \in released

EveryPresentDecided ==
  \A s \in Sources : s \in presentBearing ~>
    s \in presentPublished \cup presentSkipped

EveryPresentSettled ==
  \A s \in Sources : s \in presentBearing ~> s \in presentSettled

====
