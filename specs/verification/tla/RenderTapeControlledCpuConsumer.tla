---- MODULE RenderTapeControlledCpuConsumer ----
(******************************************************************************
 * R-HARN-REPLAY-9.18 / R-VERIF-1.1, 1.2, 1.4, 1.6, 1.7 bounded consumer.
 *
 * One producer publishes two ordered sources through one resident slot;
 * source/storage generations change when that physical slot is reused.
 * One dedicated consumer observes actual Ready work, copies owned metadata
 * outside the scheduling lock, then performs ImmediateFifo controlled
 * submission, acknowledgement and retirement. Producer capacity waits do
 * not block this consumer. Close/Stop never publish an unfinished writer.
 *
 * Model/code binding: tests/native/backend/render_tape_cpu_v4_consumer.hpp
 * cpuV4ConsumerStateValid / cpuV4ConsumerAdvance actions Start, Observe,
 * Submit, Acknowledge, Retire, CloseProducer, Stop, EmptyWake and Fail map
 * to the equally named controller actions below. BackendObserve,
 * CopyObservation, BackendSubmit, BackendAcknowledge and BackendRetire map
 * to observeReady's generation-pinned metadata borrow, submit, acknowledge
 * and retire. Each backend effect precedes its reducer commit, permitting
 * failure after the real side effect but before controller confirmation.
 * SourceCleanup/ExitCleanup/FailureCleanup bind finishCleanup / cleanup;
 * PoisonWake, ReadyWake, FailureWake and Join bind poison, waitReady,
 * requestStop, workerExited and join. Producer actions abstract natural
 * queue publication and its publication/capacity CV predicates, not a raw
 * event, original reference cut, captured watermark or forced flush.
 *
 * Failed represented/submitted/acknowledged residue is retained by the
 * poisoned queue with exact identity and actual lifecycle stage. Releasing
 * a transient borrow and recording quarantine do NOT acknowledge, retire,
 * reclaim or set cleanupComplete. Ready/Writing backlog is also queue-owned;
 * it is distinct from an unhandled consumer claim. Main joins before fixture
 * destruction; destruction of quarantined residue is outside this model.
 *
 * ProgressSpec explicitly assumes weak fairness of enabled producer,
 * publication-notification, consumer/backend, failure-wake, exit and join
 * actions. Normal liveness additionally assumes no Fail transition. Failure
 * need not clean a retained claim successfully, but must wake, exit and join.
 * No fairness assumption creates an acknowledgement after failure.
 * AllowMissingPublicationWake removes only the publication notification,
 * retaining Ready publication and safety. Its temporal negative contains a
 * consumer parked before publication and a producer blocked by the full slot.
 *
 * Exclusions: GPU/Metal execution or completion, Encode topology, numeric
 * query/pixel authority, full state/resource payload digests, allocator
 * implementation, unbounded sources, clock/deadline guarantees and timing.
 * CPU acknowledgement below is actual controlled backend evidence only.
 ******************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS MaxSources, ResidentSlots, AllowMissingPublicationWake
ASSUME MaxSources = 2 /\ ResidentSlots = 1
ASSUME AllowMissingPublicationWake \in BOOLEAN

Sources == 1 .. MaxSources
Binding == [queue |-> 71, captureToken |-> 72, captureEpoch |-> 73,
            baselineSequence |-> 0]
NoIdentity == [queue |-> 0, captureToken |-> 0, captureEpoch |-> 0,
               sequence |-> 0, sourceIndex |-> 0, sourceGeneration |-> 0,
               storageFirstPage |-> 0, storagePageCount |-> 0,
               storageGeneration |-> 0]
Identity(s) == [queue |-> Binding.queue, captureToken |-> Binding.captureToken,
                captureEpoch |-> Binding.captureEpoch, sequence |-> s,
                sourceIndex |-> 0, sourceGeneration |-> s,
                storageFirstPage |-> 0, storagePageCount |-> 1,
                storageGeneration |-> s]
NoObservation == [identity |-> NoIdentity, rawOrdinal |-> 0,
                  sourceOrdinal |-> 0, commands |-> <<>>]
Observation(s) == [identity |-> Identity(s), rawOrdinal |-> 11,
                   sourceOrdinal |-> s,
                   commands |-> <<[kind |-> s, drawCount |-> s + 2]>>]
Identities == {NoIdentity} \cup {Identity(s) : s \in Sources}
Observations == {NoObservation} \cup {Observation(s) : s \in Sources}
Bound(i) == i \in Identities \ {NoIdentity}

ControllerPhases == {"Idle", "Running", "Stopping", "Stopped", "Failed"}
SourcePhases == {"Empty", "Observed", "Submitted", "Acknowledged"}
BackendPhases == {"Absent", "Writing", "Ready", "Represented",
                  "Submitted", "Acknowledged", "Retired"}
ResidentPhases == {"Writing", "Ready", "Represented", "Submitted", "Acknowledged"}
WorkerPcs == {"Idle", "WaitReady", "Observe", "CopyObservation", "ObserveCommit",
              "Submit", "SubmitCommit", "Acknowledge", "AcknowledgeCommit",
              "Retire", "RetireCommit", "SourceCleanup", "PublishReceipt",
              "ExitCleanup", "EmptyWake", "FailureCleanup", "Exit", "Exited"}

VARIABLES control, transport, producer, worker, history
vars == <<control, transport, producer, worker, history>>

ResidentSources == {s \in Sources : transport.status[s] \in ResidentPhases}
ResidentCount == Cardinality(ResidentSources)
LiveController == control.phase \in {"Running", "Stopping"}
OrderedPrefix(xs) == /\ Len(xs) <= MaxSources
                     /\ \A n \in 1 .. Len(xs) : xs[n] = n
Members(xs) == {xs[n] : n \in 1 .. Len(xs)}

Init ==
  /\ control = [phase |-> "Idle", sourcePhase |-> "Empty", active |-> NoIdentity,
                retiredSequence |-> 0, observed |-> 0, submitted |-> 0,
                acknowledged |-> 0, retired |-> 0, commands |-> 0,
                lifecycleActions |-> 0, producerClosed |-> FALSE]
  /\ transport = [ready |-> <<>>, claimed |-> 0,
                  status |-> [s \in Sources |-> "Absent"], borrow |-> FALSE,
                  poisoned |-> FALSE, stopRequested |-> FALSE,
                  publicationWakePending |-> FALSE, wakeGeneration |-> 0,
                  capacityWakeGeneration |-> 0, quarantined |-> {}]
  /\ producer = [nextSource |-> 1, writer |-> 0, closed |-> FALSE,
                 blocked |-> FALSE, sampledWake |-> 0]
  /\ worker = [pc |-> "Idle", waiting |-> FALSE, sampledWake |-> 0,
               snapshot |-> NoObservation, cleanupComplete |-> FALSE,
               cleanupFailed |-> FALSE, exited |-> FALSE, joined |-> FALSE]
  /\ history = [published |-> <<>>, observed |-> <<>>, submitted |-> <<>>,
                acknowledged |-> <<>>, retired |-> <<>>, receipts |-> <<>>]

Start ==
  /\ control.phase = "Idle"
  /\ control' = [control EXCEPT !.phase = "Running"]
  /\ worker' = [worker EXCEPT !.pc = "WaitReady"]
  /\ UNCHANGED <<transport, producer, history>>

BeginWriting ==
  /\ control.phase = "Running" /\ ~producer.closed
  /\ producer.nextSource \in Sources /\ producer.writer = 0 /\ ~producer.blocked
  /\ ResidentCount < ResidentSlots
  /\ transport.status[producer.nextSource] = "Absent"
  /\ transport' = [transport EXCEPT !.status[producer.nextSource] = "Writing"]
  /\ producer' = [producer EXCEPT !.writer = producer.nextSource]
  /\ UNCHANGED <<control, worker, history>>

ParkProducer ==
  /\ control.phase = "Running" /\ producer.nextSource \in Sources
  /\ producer.writer = 0 /\ ~producer.blocked /\ ResidentCount = ResidentSlots
  /\ producer' = [producer EXCEPT !.blocked = TRUE,
                    !.sampledWake = transport.capacityWakeGeneration]
  /\ UNCHANGED <<control, transport, worker, history>>

RetryProducer ==
  /\ control.phase = "Running" /\ producer.blocked
  /\ ResidentCount < ResidentSlots
  /\ transport.capacityWakeGeneration > producer.sampledWake
  /\ producer' = [producer EXCEPT !.blocked = FALSE]
  /\ UNCHANGED <<control, transport, worker, history>>

Publish ==
  /\ control.phase = "Running" /\ ~producer.closed
  /\ producer.writer \in Sources /\ transport.status[producer.writer] = "Writing"
  /\ transport' = [transport EXCEPT !.status[producer.writer] = "Ready",
                    !.ready = Append(@, producer.writer), !.publicationWakePending = TRUE]
  /\ history' = [history EXCEPT !.published = Append(@, producer.writer)]
  /\ producer' = [producer EXCEPT !.writer = 0, !.nextSource = @ + 1]
  /\ UNCHANGED <<control, worker>>

PublicationWake ==
  /\ transport.publicationWakePending
  /\ transport' = [transport EXCEPT !.publicationWakePending = FALSE,
                    !.wakeGeneration = IF AllowMissingPublicationWake THEN @ ELSE @ + 1]
  /\ UNCHANGED <<control, producer, worker, history>>

ParkConsumer ==
  /\ LiveController /\ worker.pc = "WaitReady" /\ ~worker.waiting
  /\ Len(transport.ready) = 0 /\ ~transport.stopRequested
  /\ worker' = [worker EXCEPT !.waiting = TRUE, !.sampledWake = transport.wakeGeneration]
  /\ UNCHANGED <<control, transport, producer, history>>

ReadyWake ==
  /\ LiveController /\ worker.pc = "WaitReady" /\ Len(transport.ready) > 0
  /\ ~worker.waiting \/ transport.wakeGeneration > worker.sampledWake
  /\ worker' = [worker EXCEPT !.pc = "Observe", !.waiting = FALSE]
  /\ UNCHANGED <<control, transport, producer, history>>

BackendObserve ==
  LET s == Head(transport.ready) IN
  /\ LiveController /\ worker.pc = "Observe" /\ Len(transport.ready) > 0
  /\ transport.claimed = 0 /\ transport.status[s] = "Ready"
  /\ control.observed < MaxSources /\ control.commands < MaxSources
  /\ transport' = [transport EXCEPT !.ready = Tail(@), !.claimed = s,
                    !.status[s] = "Represented", !.borrow = TRUE]
  /\ history' = [history EXCEPT !.observed = Append(@, s)]
  /\ worker' = [worker EXCEPT !.pc = "CopyObservation", !.cleanupComplete = FALSE]
  /\ UNCHANGED <<control, producer>>

CopyObservation ==
  /\ LiveController /\ worker.pc = "CopyObservation" /\ transport.claimed \in Sources
  /\ transport.borrow
  /\ worker' = [worker EXCEPT !.snapshot = Observation(transport.claimed), !.pc = "ObserveCommit"]
  /\ transport' = [transport EXCEPT !.borrow = FALSE]
  /\ UNCHANGED <<control, producer, history>>

Observe ==
  /\ LiveController /\ worker.pc = "ObserveCommit" /\ control.sourcePhase = "Empty"
  /\ Bound(worker.snapshot.identity) /\ worker.snapshot.identity.sequence > control.retiredSequence
  /\ worker.snapshot = Observation(transport.claimed)
  /\ control.lifecycleActions < 4 * MaxSources
  /\ control' = [control EXCEPT !.sourcePhase = "Observed", !.active = worker.snapshot.identity,
                  !.observed = @ + 1, !.commands = @ + 1, !.lifecycleActions = @ + 1]
  /\ worker' = [worker EXCEPT !.pc = "Submit"]
  /\ UNCHANGED <<transport, producer, history>>

BackendSubmit ==
  /\ LiveController /\ worker.pc = "Submit" /\ control.sourcePhase = "Observed"
  /\ transport.claimed \in Sources /\ control.active = Identity(transport.claimed)
  /\ transport.status[transport.claimed] = "Represented"
  /\ transport' = [transport EXCEPT !.status[transport.claimed] = "Submitted"]
  /\ history' = [history EXCEPT !.submitted = Append(@, transport.claimed)]
  /\ worker' = [worker EXCEPT !.pc = "SubmitCommit"]
  /\ UNCHANGED <<control, producer>>

Submit ==
  /\ LiveController /\ worker.pc = "SubmitCommit" /\ control.sourcePhase = "Observed"
  /\ control.active = worker.snapshot.identity /\ control.lifecycleActions < 4 * MaxSources
  /\ control' = [control EXCEPT !.sourcePhase = "Submitted", !.submitted = @ + 1,
                  !.lifecycleActions = @ + 1]
  /\ worker' = [worker EXCEPT !.pc = "Acknowledge"]
  /\ UNCHANGED <<transport, producer, history>>

BackendAcknowledge ==
  /\ LiveController /\ worker.pc = "Acknowledge" /\ control.sourcePhase = "Submitted"
  /\ transport.claimed \in Sources /\ control.active = Identity(transport.claimed)
  /\ transport.status[transport.claimed] = "Submitted"
  /\ transport' = [transport EXCEPT !.status[transport.claimed] = "Acknowledged"]
  /\ history' = [history EXCEPT !.acknowledged = Append(@, transport.claimed)]
  /\ worker' = [worker EXCEPT !.pc = "AcknowledgeCommit"]
  /\ UNCHANGED <<control, producer>>

Acknowledge ==
  /\ LiveController /\ worker.pc = "AcknowledgeCommit" /\ control.sourcePhase = "Submitted"
  /\ control.active = worker.snapshot.identity /\ control.lifecycleActions < 4 * MaxSources
  /\ control' = [control EXCEPT !.sourcePhase = "Acknowledged", !.acknowledged = @ + 1,
                  !.lifecycleActions = @ + 1]
  /\ worker' = [worker EXCEPT !.pc = "Retire"]
  /\ UNCHANGED <<transport, producer, history>>

BackendRetire ==
  /\ LiveController /\ worker.pc = "Retire" /\ control.sourcePhase = "Acknowledged"
  /\ transport.claimed \in Sources /\ control.active = Identity(transport.claimed)
  /\ transport.status[transport.claimed] = "Acknowledged"
  /\ history' = [history EXCEPT !.retired = Append(@, transport.claimed)]
  /\ transport' = [transport EXCEPT !.status[transport.claimed] = "Retired", !.claimed = 0,
                    !.capacityWakeGeneration = @ + 1]
  /\ worker' = [worker EXCEPT !.pc = "RetireCommit"]
  /\ UNCHANGED <<control, producer>>

Retire ==
  /\ LiveController /\ worker.pc = "RetireCommit" /\ control.sourcePhase = "Acknowledged"
  /\ control.active = worker.snapshot.identity /\ control.lifecycleActions < 4 * MaxSources
  /\ control' = [control EXCEPT !.sourcePhase = "Empty", !.active = NoIdentity,
                  !.retiredSequence = worker.snapshot.identity.sequence,
                  !.retired = @ + 1, !.lifecycleActions = @ + 1]
  /\ worker' = [worker EXCEPT !.pc = "SourceCleanup"]
  /\ UNCHANGED <<transport, producer, history>>

SourceCleanup ==
  /\ LiveController /\ worker.pc = "SourceCleanup" /\ transport.claimed = 0
  /\ worker' = [worker EXCEPT !.cleanupComplete = TRUE, !.pc = "PublishReceipt"]
  /\ UNCHANGED <<control, transport, producer, history>>

PublishReceipt ==
  /\ LiveController /\ worker.pc = "PublishReceipt" /\ worker.cleanupComplete
  /\ worker.snapshot.identity.sequence = Len(history.receipts) + 1
  /\ history' = [history EXCEPT !.receipts = Append(@, worker.snapshot)]
  /\ worker' = [worker EXCEPT !.snapshot = NoObservation, !.pc = "WaitReady"]
  /\ UNCHANGED <<control, transport, producer>>

CloseProducer ==
  /\ control.phase = "Running" /\ ~control.producerClosed /\ ~producer.closed
  /\ producer.nextSource = MaxSources + 1 /\ producer.writer = 0
  /\ control' = [control EXCEPT !.producerClosed = TRUE]
  /\ producer' = [producer EXCEPT !.closed = TRUE]
  /\ UNCHANGED <<transport, worker, history>>

Stop ==
  /\ control.phase = "Running" /\ control.producerClosed
  /\ producer.closed /\ producer.writer = 0
  /\ control' = [control EXCEPT !.phase = "Stopping"]
  /\ UNCHANGED <<transport, producer, worker, history>>

RequestStopWake ==
  /\ control.phase = "Stopping" /\ ~transport.stopRequested
  /\ transport' = [transport EXCEPT !.stopRequested = TRUE, !.wakeGeneration = @ + 1]
  /\ UNCHANGED <<control, producer, worker, history>>

StoppedWake ==
  /\ control.phase = "Stopping" /\ worker.pc = "WaitReady" /\ transport.stopRequested
  /\ producer.closed /\ producer.writer = 0 /\ Len(transport.ready) = 0 /\ transport.claimed = 0
  /\ ~worker.waiting \/ transport.wakeGeneration > worker.sampledWake
  /\ worker' = [worker EXCEPT !.pc = "ExitCleanup", !.waiting = FALSE]
  /\ UNCHANGED <<control, transport, producer, history>>

ExitCleanup ==
  /\ control.phase = "Stopping" /\ worker.pc = "ExitCleanup" /\ transport.claimed = 0
  /\ worker' = [worker EXCEPT !.cleanupComplete = TRUE, !.pc = "EmptyWake"]
  /\ UNCHANGED <<control, transport, producer, history>>

EmptyWake ==
  /\ control.phase = "Stopping" /\ worker.pc = "EmptyWake" /\ worker.cleanupComplete
  /\ control.producerClosed /\ control.sourcePhase = "Empty" /\ control.observed = control.retired
  /\ control' = [control EXCEPT !.phase = "Stopped"]
  /\ worker' = [worker EXCEPT !.pc = "Exit"]
  /\ UNCHANGED <<transport, producer, history>>

Fail ==
  /\ control.phase \in {"Idle", "Running", "Stopping"}
  /\ control' = [control EXCEPT !.phase = "Failed"]
  /\ UNCHANGED <<transport, producer, worker, history>>

PoisonWake ==
  /\ control.phase = "Failed" /\ ~transport.poisoned
  /\ transport' = [transport EXCEPT !.poisoned = TRUE, !.quarantined = ResidentSources,
                    !.wakeGeneration = @ + 1, !.capacityWakeGeneration = @ + 1]
  /\ UNCHANGED <<control, producer, worker, history>>

FailureWake ==
  /\ control.phase = "Failed" /\ transport.poisoned /\ ~worker.exited
  /\ worker.pc \notin {"FailureCleanup", "Exit"}
  /\ ~worker.waiting \/ transport.wakeGeneration > worker.sampledWake
  /\ worker' = [worker EXCEPT !.waiting = FALSE, !.pc = "FailureCleanup"]
  /\ UNCHANGED <<control, transport, producer, history>>

FailureCleanup ==
  /\ control.phase = "Failed" /\ transport.poisoned /\ worker.pc = "FailureCleanup"
  /\ transport' = [transport EXCEPT !.borrow = FALSE]
  /\ worker' = [worker EXCEPT !.cleanupComplete = (transport.claimed = 0),
                !.cleanupFailed = @ \/ (transport.claimed # 0), !.pc = "Exit"]
  /\ UNCHANGED <<control, producer, history>>

AbortProducerWait ==
  /\ control.phase = "Failed" /\ transport.poisoned /\ producer.blocked
  /\ transport.capacityWakeGeneration > producer.sampledWake
  /\ producer' = [producer EXCEPT !.blocked = FALSE]
  /\ UNCHANGED <<control, transport, worker, history>>

WorkerExit ==
  /\ control.phase \in {"Stopped", "Failed"} /\ worker.pc = "Exit"
  /\ ~transport.borrow
  /\ worker' = [worker EXCEPT !.exited = TRUE, !.pc = "Exited"]
  /\ UNCHANGED <<control, transport, producer, history>>

Join ==
  /\ worker.exited /\ ~worker.joined
  /\ worker' = [worker EXCEPT !.joined = TRUE]
  /\ UNCHANGED <<control, transport, producer, history>>

Next == Start \/ BeginWriting \/ ParkProducer \/ RetryProducer \/ Publish \/ PublicationWake
     \/ ParkConsumer \/ ReadyWake \/ BackendObserve \/ CopyObservation \/ Observe
     \/ BackendSubmit \/ Submit \/ BackendAcknowledge \/ Acknowledge \/ BackendRetire \/ Retire
     \/ SourceCleanup \/ PublishReceipt \/ CloseProducer \/ Stop \/ RequestStopWake
     \/ StoppedWake \/ ExitCleanup \/ EmptyWake \/ Fail \/ PoisonWake \/ FailureWake
     \/ FailureCleanup \/ AbortProducerWait \/ WorkerExit \/ Join

TypeOK ==
  /\ control.phase \in ControllerPhases /\ control.sourcePhase \in SourcePhases
  /\ control.active \in Identities /\ control.retiredSequence \in 0 .. MaxSources
  /\ control.observed \in 0 .. MaxSources /\ control.submitted \in 0 .. MaxSources
  /\ control.acknowledged \in 0 .. MaxSources /\ control.retired \in 0 .. MaxSources
  /\ control.commands \in 0 .. MaxSources /\ control.lifecycleActions \in 0 .. (4 * MaxSources)
  /\ control.producerClosed \in BOOLEAN
  /\ transport.ready \in Seq(Sources) /\ transport.claimed \in Sources \cup {0}
  /\ transport.status \in [Sources -> BackendPhases] /\ transport.borrow \in BOOLEAN
  /\ transport.poisoned \in BOOLEAN /\ transport.stopRequested \in BOOLEAN
  /\ transport.publicationWakePending \in BOOLEAN
  /\ transport.wakeGeneration \in 0 .. (MaxSources + 2)
  /\ transport.capacityWakeGeneration \in 0 .. (MaxSources + 1)
  /\ transport.quarantined \subseteq Sources
  /\ producer.nextSource \in 1 .. (MaxSources + 1) /\ producer.writer \in Sources \cup {0}
  /\ producer.closed \in BOOLEAN /\ producer.blocked \in BOOLEAN
  /\ producer.sampledWake \in 0 .. (MaxSources + 1)
  /\ worker.pc \in WorkerPcs /\ worker.waiting \in BOOLEAN
  /\ worker.sampledWake \in 0 .. (MaxSources + 2) /\ worker.snapshot \in Observations
  /\ worker.cleanupComplete \in BOOLEAN /\ worker.cleanupFailed \in BOOLEAN
  /\ worker.exited \in BOOLEAN /\ worker.joined \in BOOLEAN
  /\ history.published \in Seq(Sources) /\ history.observed \in Seq(Sources)
  /\ history.submitted \in Seq(Sources) /\ history.acknowledged \in Seq(Sources)
  /\ history.retired \in Seq(Sources) /\ history.receipts \in Seq(Observations)

ReducerValid ==
  /\ control.retired <= control.acknowledged /\ control.acknowledged <= control.submitted
  /\ control.submitted <= control.observed /\ control.observed - control.retired <= 1
  /\ control.commands = control.observed
  /\ control.lifecycleActions = control.observed + control.submitted + control.acknowledged + control.retired
  /\ control.retiredSequence = control.retired
  /\ (control.sourcePhase = "Empty" => /\ control.active = NoIdentity
                                        /\ control.observed = control.retired)
  /\ (control.sourcePhase = "Observed" => /\ control.observed = control.retired + 1
                                           /\ control.submitted = control.retired)
  /\ (control.sourcePhase = "Submitted" => /\ control.observed = control.retired + 1
                                            /\ control.submitted = control.observed
                                            /\ control.acknowledged = control.retired)
  /\ (control.sourcePhase = "Acknowledged" => /\ control.observed = control.retired + 1
                                               /\ control.acknowledged = control.observed)
  /\ (control.sourcePhase # "Empty" => /\ Bound(control.active)
                                        /\ control.active.sequence > control.retiredSequence)

CapacityAndOwnership ==
  /\ ResidentCount <= ResidentSlots
  /\ Len(transport.ready) <= ResidentSlots
  /\ \A n \in 1 .. Len(transport.ready) : transport.status[transport.ready[n]] = "Ready"
  /\ (transport.claimed # 0 => transport.status[transport.claimed] \in
          {"Represented", "Submitted", "Acknowledged"})
  /\ (producer.writer # 0 => transport.status[producer.writer] = "Writing")
  /\ (transport.borrow => transport.claimed # 0)
  /\ (worker.snapshot # NoObservation => worker.snapshot = Observation(worker.snapshot.sourceOrdinal))

ActualLifecycleAuthority ==
  /\ OrderedPrefix(history.published) /\ OrderedPrefix(history.observed)
  /\ OrderedPrefix(history.submitted) /\ OrderedPrefix(history.acknowledged)
  /\ OrderedPrefix(history.retired)
  /\ Len(history.retired) <= Len(history.acknowledged)
  /\ Len(history.acknowledged) <= Len(history.submitted)
  /\ Len(history.submitted) <= Len(history.observed)
  /\ Len(history.observed) <= Len(history.published)
  /\ control.observed <= Len(history.observed) /\ control.submitted <= Len(history.submitted)
  /\ control.acknowledged <= Len(history.acknowledged) /\ control.retired <= Len(history.retired)
  /\ Len(history.receipts) <= control.retired
  /\ \A n \in 1 .. Len(history.receipts) : history.receipts[n] = Observation(n)

BackendStagesAreActual ==
  \A s \in Sources :
    /\ (transport.status[s] \in {"Ready", "Represented", "Submitted", "Acknowledged", "Retired"}
          <=> s \in Members(history.published))
    /\ (transport.status[s] \in {"Represented", "Submitted", "Acknowledged", "Retired"}
          <=> s \in Members(history.observed))
    /\ (transport.status[s] \in {"Submitted", "Acknowledged", "Retired"}
          <=> s \in Members(history.submitted))
    /\ (transport.status[s] \in {"Acknowledged", "Retired"}
          <=> s \in Members(history.acknowledged))
    /\ (transport.status[s] = "Retired" <=> s \in Members(history.retired))

TruthfulCleanupAndQuarantine ==
  /\ (worker.cleanupComplete => transport.claimed = 0 /\ ~transport.borrow)
  /\ (transport.poisoned => transport.quarantined = ResidentSources)
  /\ (worker.exited /\ transport.claimed # 0 =>
          /\ control.phase = "Failed" /\ transport.poisoned
          /\ ~worker.cleanupComplete /\ worker.cleanupFailed
          /\ transport.claimed \in transport.quarantined)
  /\ (worker.joined => worker.exited /\ ~transport.borrow)

NormalStopRequiresEmptyWriter ==
  /\ (control.producerClosed => producer.closed /\ producer.writer = 0)
  /\ (control.phase = "Stopping" => control.producerClosed)
  /\ (control.phase = "Stopped" =>
          /\ control.producerClosed /\ control.sourcePhase = "Empty"
          /\ control.observed = control.retired /\ worker.cleanupComplete
          /\ ResidentCount = 0 /\ transport.claimed = 0 /\ Len(transport.ready) = 0)

Inv == TypeOK /\ ReducerValid /\ CapacityAndOwnership /\ ActualLifecycleAuthority
       /\ BackendStagesAreActual /\ TruthfulCleanupAndQuarantine /\ NormalStopRequiresEmptyWriter

Spec == Init /\ [][Next]_vars

ProgressSpec == Spec
  /\ WF_vars(Start) /\ WF_vars(BeginWriting) /\ WF_vars(Publish)
  /\ WF_vars(PublicationWake) /\ WF_vars(RetryProducer) /\ WF_vars(ReadyWake)
  /\ WF_vars(BackendObserve) /\ WF_vars(CopyObservation) /\ WF_vars(Observe)
  /\ WF_vars(BackendSubmit) /\ WF_vars(Submit)
  /\ WF_vars(BackendAcknowledge) /\ WF_vars(Acknowledge)
  /\ WF_vars(BackendRetire) /\ WF_vars(Retire)
  /\ WF_vars(SourceCleanup) /\ WF_vars(PublishReceipt)
  /\ WF_vars(CloseProducer) /\ WF_vars(Stop) /\ WF_vars(RequestStopWake)
  /\ WF_vars(StoppedWake) /\ WF_vars(ExitCleanup) /\ WF_vars(EmptyWake)
  /\ WF_vars(PoisonWake) /\ WF_vars(FailureWake) /\ WF_vars(FailureCleanup)
  /\ WF_vars(AbortProducerWait) /\ WF_vars(WorkerExit) /\ WF_vars(Join)

NormalDrainAndJoin ==
  ([](control.phase # "Failed")) =>
    <>(worker.joined /\ control.phase = "Stopped" /\ Len(history.receipts) = MaxSources)
FailureWakeExitAndJoin ==
  [](control.phase = "Failed" ~> (transport.poisoned /\ worker.joined /\ ~producer.blocked))
ProducerCapacityProgress ==
  [](producer.blocked ~> (~producer.blocked \/ control.phase = "Failed"))

=============================================================================
