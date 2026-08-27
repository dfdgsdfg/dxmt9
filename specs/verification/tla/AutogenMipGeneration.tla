---- MODULE AutogenMipGeneration ----
(***************************************************************************
 * R-FORMAT-16 automatic-mipmap generation refinement.
 *
 * D3D9 exposes only level 0 of an AUTOGENMIPMAP texture.  The PE object owns
 * the logical Dirty/Clean state, while the unix provider owns the complete
 * hidden Metal pyramid.  A sampling draw may therefore observe the texture
 * only after every preceding level-0 write has been published, drained, and
 * used as the source of a successful physical mip generation.  A failed
 * generation settles the current draw request without publishing Clean, so a
 * later draw retries rather than sampling stale hidden levels.
 *************************************************************************)

EXTENDS Naturals, TLC

CONSTANTS MaxWrites, MaxSamples

VARIABLES dirty, lastWrite, publishedWrite, generatedWrite, requestPhase,
          sampledWrite, sampleCount, staleSample, generationFailures

RequestPhases == {"Idle", "Publishing", "Generating", "Ready"}

vars == <<dirty, lastWrite, publishedWrite, generatedWrite, requestPhase,
          sampledWrite, sampleCount, staleSample, generationFailures>>

Init ==
  /\ dirty = FALSE
  /\ lastWrite = 0
  /\ publishedWrite = 0
  /\ generatedWrite = 0
  /\ requestPhase = "Idle"
  /\ sampledWrite = 0
  /\ sampleCount = 0
  /\ staleSample = FALSE
  /\ generationFailures = FALSE

LevelZeroWrite ==
  /\ requestPhase = "Idle"
  /\ lastWrite < MaxWrites
  /\ lastWrite' = lastWrite + 1
  /\ dirty' = TRUE
  /\ UNCHANGED <<publishedWrite, generatedWrite, requestPhase, sampledWrite,
                  sampleCount, staleSample, generationFailures>>

RequestDirtySample ==
  /\ requestPhase = "Idle"
  /\ dirty
  /\ sampleCount < MaxSamples
  /\ requestPhase' = "Publishing"
  /\ UNCHANGED <<dirty, lastWrite, publishedWrite, generatedWrite,
                  sampledWrite, sampleCount, staleSample,
                  generationFailures>>

PublishPriorWrites ==
  /\ requestPhase = "Publishing"
  /\ publishedWrite' = lastWrite
  /\ requestPhase' = "Generating"
  /\ UNCHANGED <<dirty, lastWrite, generatedWrite, sampledWrite,
                  sampleCount, staleSample, generationFailures>>

GenerationSucceeded ==
  /\ requestPhase = "Generating"
  /\ generatedWrite' = publishedWrite
  /\ dirty' = FALSE
  /\ requestPhase' = "Ready"
  /\ UNCHANGED <<lastWrite, publishedWrite, sampledWrite, sampleCount,
                  staleSample, generationFailures>>

GenerationFailed ==
  /\ requestPhase = "Generating"
  /\ dirty' = TRUE
  /\ generationFailures' = TRUE
  /\ requestPhase' = "Idle"
  /\ UNCHANGED <<lastWrite, publishedWrite, generatedWrite, sampledWrite,
                  sampleCount, staleSample>>

SampleReady ==
  /\ requestPhase = "Ready"
  /\ sampleCount < MaxSamples
  /\ sampledWrite' = generatedWrite
  /\ sampleCount' = sampleCount + 1
  /\ staleSample' = staleSample \/ generatedWrite # lastWrite
  /\ requestPhase' = "Idle"
  /\ UNCHANGED <<dirty, lastWrite, publishedWrite, generatedWrite,
                  generationFailures>>

SampleClean ==
  /\ requestPhase = "Idle"
  /\ ~dirty
  /\ sampleCount < MaxSamples
  /\ sampledWrite' = generatedWrite
  /\ sampleCount' = sampleCount + 1
  /\ staleSample' = staleSample \/ generatedWrite # lastWrite
  /\ UNCHANGED <<dirty, lastWrite, publishedWrite, generatedWrite,
                  requestPhase, generationFailures>>

Next ==
  \/ LevelZeroWrite
  \/ RequestDirtySample
  \/ PublishPriorWrites
  \/ GenerationSucceeded
  \/ GenerationFailed
  \/ SampleReady
  \/ SampleClean

Spec ==
  /\ Init
  /\ [][Next]_vars
  /\ WF_vars(PublishPriorWrites)
  /\ WF_vars(GenerationSucceeded \/ GenerationFailed)
  /\ WF_vars(SampleReady)

TypeOK ==
  /\ dirty \in BOOLEAN
  /\ lastWrite \in 0 .. MaxWrites
  /\ publishedWrite \in 0 .. MaxWrites
  /\ generatedWrite \in 0 .. MaxWrites
  /\ requestPhase \in RequestPhases
  /\ sampledWrite \in 0 .. MaxWrites
  /\ sampleCount \in 0 .. MaxSamples
  /\ staleSample \in BOOLEAN
  /\ generationFailures \in BOOLEAN

PublishedNeverAhead == publishedWrite <= lastWrite
GeneratedNeverAhead == generatedWrite <= publishedWrite
CleanCoversLatestWrite == ~dirty => generatedWrite = lastWrite
SampleNeverUsesStaleMip == ~staleSample
ReadyHasExactGeneration == requestPhase = "Ready" => generatedWrite = lastWrite

Inv ==
  /\ TypeOK
  /\ PublishedNeverAhead
  /\ GeneratedNeverAhead
  /\ CleanCoversLatestWrite
  /\ SampleNeverUsesStaleMip
  /\ ReadyHasExactGeneration

RequestEventuallySettles == requestPhase # "Idle" ~> requestPhase = "Idle"

=============================================================================
