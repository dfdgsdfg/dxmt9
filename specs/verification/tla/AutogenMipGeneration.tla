---- MODULE AutogenMipGeneration ----
(***************************************************************************
 * R-FORMAT-16 automatic-mipmap generation refinement.
 *
 * D3D9 exposes only level 0 of an AUTOGENMIPMAP texture.  The PE object owns
 * the logical Dirty/Clean state, while the unix provider owns the complete
 * hidden Metal pyramid.  A sampling draw may therefore observe the texture
 * only after an ordered GenerateMipmaps record for every preceding level-0
 * write has replayed successfully before that draw in the same submission.
 * PE marks its recorder-local state Clean when the retained record is
 * accepted, not when Metal finishes.  A later replay/encode failure is
 * therefore fail-stop: the device enters Fatal and no sampling draw may
 * observe the optimistic Clean state.
 *************************************************************************)

EXTENDS Naturals, TLC

CONSTANTS MaxWrites, MaxSamples

VARIABLES dirty, lastWrite, queuedWrite, generatedWrite, requestPhase,
          sampledWrite, sampleCount, staleSample, appendFailures, fatal

RequestPhases == {"Idle", "Queued", "Generating", "Ready", "Fatal"}

vars == <<dirty, lastWrite, queuedWrite, generatedWrite, requestPhase,
          sampledWrite, sampleCount, staleSample, appendFailures, fatal>>

Init ==
  /\ dirty = FALSE
  /\ lastWrite = 0
  /\ queuedWrite = 0
  /\ generatedWrite = 0
  /\ requestPhase = "Idle"
  /\ sampledWrite = 0
  /\ sampleCount = 0
  /\ staleSample = FALSE
  /\ appendFailures = FALSE
  /\ fatal = FALSE

LevelZeroWrite ==
  /\ requestPhase = "Idle"
  /\ lastWrite < MaxWrites
  /\ lastWrite' = lastWrite + 1
  /\ dirty' = TRUE
  /\ UNCHANGED <<queuedWrite, generatedWrite, requestPhase, sampledWrite,
                  sampleCount, staleSample, appendFailures, fatal>>

RequestDirtySample ==
  /\ requestPhase = "Idle"
  /\ dirty
  /\ sampleCount < MaxSamples
  /\ queuedWrite' = lastWrite
  /\ dirty' = FALSE
  /\ requestPhase' = "Queued"
  /\ UNCHANGED <<lastWrite, generatedWrite, sampledWrite, sampleCount,
                  staleSample, appendFailures, fatal>>

RecordAppendFailed ==
  /\ requestPhase = "Idle"
  /\ dirty
  /\ sampleCount < MaxSamples
  /\ ~appendFailures
  /\ appendFailures' = TRUE
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, generatedWrite, requestPhase,
                  sampledWrite, sampleCount, staleSample, fatal>>

ReplayQueuedRecord ==
  /\ requestPhase = "Queued"
  /\ requestPhase' = "Generating"
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, generatedWrite, sampledWrite,
                  sampleCount, staleSample, appendFailures, fatal>>

GenerationSucceeded ==
  /\ requestPhase = "Generating"
  /\ generatedWrite' = queuedWrite
  /\ requestPhase' = "Ready"
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, sampledWrite, sampleCount,
                  staleSample, appendFailures, fatal>>

GenerationFailed ==
  /\ requestPhase = "Generating"
  /\ fatal' = TRUE
  /\ requestPhase' = "Fatal"
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, generatedWrite, sampledWrite,
                  sampleCount, staleSample, appendFailures>>

SampleReady ==
  /\ requestPhase = "Ready"
  /\ sampleCount < MaxSamples
  /\ sampledWrite' = generatedWrite
  /\ sampleCount' = sampleCount + 1
  /\ staleSample' = staleSample \/ generatedWrite # lastWrite
  /\ requestPhase' = "Idle"
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, generatedWrite,
                  appendFailures, fatal>>

SampleClean ==
  /\ requestPhase = "Idle"
  /\ ~dirty
  /\ sampleCount < MaxSamples
  /\ sampledWrite' = generatedWrite
  /\ sampleCount' = sampleCount + 1
  /\ staleSample' = staleSample \/ generatedWrite # lastWrite
  /\ UNCHANGED <<dirty, lastWrite, queuedWrite, generatedWrite,
                  requestPhase, appendFailures, fatal>>

Next ==
  \/ LevelZeroWrite
  \/ RequestDirtySample
  \/ RecordAppendFailed
  \/ ReplayQueuedRecord
  \/ GenerationSucceeded
  \/ GenerationFailed
  \/ SampleReady
  \/ SampleClean

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(ReplayQueuedRecord)
  /\ WF_vars(GenerationSucceeded \/ GenerationFailed)
  /\ WF_vars(SampleReady)

TypeOK ==
  /\ dirty \in BOOLEAN
  /\ lastWrite \in 0 .. MaxWrites
  /\ queuedWrite \in 0 .. MaxWrites
  /\ generatedWrite \in 0 .. MaxWrites
  /\ requestPhase \in RequestPhases
  /\ sampledWrite \in 0 .. MaxWrites
  /\ sampleCount \in 0 .. MaxSamples
  /\ staleSample \in BOOLEAN
  /\ appendFailures \in BOOLEAN
  /\ fatal \in BOOLEAN

QueuedNeverAhead == queuedWrite <= lastWrite
GeneratedNeverAhead == generatedWrite <= queuedWrite
AccessibleCleanCoversLatestWrite ==
  ~dirty /\ requestPhase # "Fatal" =>
    IF requestPhase \in {"Queued", "Generating", "Ready"}
      THEN queuedWrite = lastWrite
      ELSE generatedWrite = lastWrite
SampleNeverUsesStaleMip == ~staleSample
AcceptedRequestHasExactWrite ==
  requestPhase \in {"Queued", "Generating", "Ready"} =>
    queuedWrite = lastWrite
ReadyHasExactGeneration ==
  requestPhase = "Ready" => generatedWrite = queuedWrite
FatalIsTerminal == requestPhase = "Fatal" <=> fatal

Inv ==
  /\ TypeOK
  /\ QueuedNeverAhead
  /\ GeneratedNeverAhead
  /\ AccessibleCleanCoversLatestWrite
  /\ SampleNeverUsesStaleMip
  /\ AcceptedRequestHasExactWrite
  /\ ReadyHasExactGeneration
  /\ FatalIsTerminal

RequestEventuallySettles ==
  requestPhase # "Idle" ~> requestPhase \in {"Idle", "Fatal"}

=============================================================================
