---- MODULE PeStateBlockValues ----
(***************************************************************************
 * Repeated production-bound StateBlock value model. RenderState (R) and
 * Transform (T) are frozen tracked categories, Sampler (S) is untracked,
 * and values cycle A/B/C (0/1/2). Mutation after Capture is explicit.
 ****************************************************************************)
EXTENDS Naturals, FiniteSets, TLC, PeStateBlockTransitionTable,
        PeStateBlockValueTable

CONSTANTS Mutation, MaxCycles
ASSUME Mutation \in {"Guarded", "MutableTracked", "FailedCapturePublish",
                     "StaleApply"}
ASSUME MaxCycles \in 2..3

Keys == {"R", "T", "S"}
FrozenTracked == {"R", "T"}
NextValue(value) == IF value = 2 THEN 0 ELSE value + 1

VARIABLES lifecycle, stage, tracked, live, snapshot, snapshotOrdinal,
          publishedOrdinal, cycle, captureBefore, applyBefore,
          publicationBefore, lastCaptureFailed, lastApplyAccepted,
          lastApplyFailed, captureBackendFailed

vars == <<lifecycle, stage, tracked, live, snapshot, snapshotOrdinal,
          publishedOrdinal, cycle, captureBefore, applyBefore,
          publicationBefore, lastCaptureFailed, lastApplyAccepted,
          lastApplyFailed, captureBackendFailed>>

Init ==
  /\ lifecycle = "Idle" /\ stage = "Idle" /\ tracked = FrozenTracked
  /\ live = [k \in Keys |-> 0]
  /\ snapshot = [k \in FrozenTracked |-> 0]
  /\ snapshotOrdinal = [k \in FrozenTracked |-> 0]
  /\ publishedOrdinal = [k \in FrozenTracked |-> 0]
  /\ cycle = 0 /\ captureBefore = snapshot /\ applyBefore = live
  /\ publicationBefore = publishedOrdinal /\ lastCaptureFailed = FALSE
  /\ lastApplyAccepted = FALSE /\ lastApplyFailed = FALSE
  /\ captureBackendFailed = FALSE

MutateBeforeCapture ==
  /\ lifecycle = "Idle" /\ stage = "Idle" /\ cycle < MaxCycles
  /\ live' = [k \in Keys |-> NextValue(live[k])]
  /\ stage' = "MutatedBeforeCapture" /\ captureBefore' = snapshot
  /\ lastCaptureFailed' = FALSE /\ lastApplyAccepted' = FALSE
  /\ lastApplyFailed' = FALSE
  /\ UNCHANGED <<lifecycle, tracked, snapshot, snapshotOrdinal,
                 publishedOrdinal, cycle, applyBefore, publicationBefore,
                 captureBackendFailed>>

CaptureAccepted ==
  /\ lifecycle = "Idle" /\ stage = "MutatedBeforeCapture"
  /\ StateBlockMatches("Idle", "CapturePublished", "Idle", "PublishCapture",
                       "Preserve", "Preserve", "Publish")
  /\ StateBlockValueMatches("CaptureAccepted", "PublishCapture",
                            TRUE, TRUE, FALSE, FALSE)
  /\ LET nextTracked == IF Mutation = "MutableTracked" THEN Keys
                        ELSE tracked IN
       /\ tracked' = nextTracked
       /\ snapshot' = [k \in nextTracked |-> live[k]]
       /\ snapshotOrdinal' = [k \in nextTracked |-> cycle + 1]
  /\ stage' = "Captured" /\ lastCaptureFailed' = FALSE
  /\ UNCHANGED <<lifecycle, live, publishedOrdinal, cycle, captureBefore,
                 applyBefore, publicationBefore, lastApplyAccepted,
                 lastApplyFailed, captureBackendFailed>>

CapturePreEffectFailed ==
  /\ lifecycle = "Idle" /\ stage = "MutatedBeforeCapture"
  /\ StateBlockMatches("Idle", "CapturePreEffectFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ StateBlockValueMatches("CapturePreEffectFailed", "Preserve",
                            TRUE, FALSE, FALSE, FALSE)
  /\ lastCaptureFailed' = TRUE
  /\ snapshot' = IF Mutation = "FailedCapturePublish"
                    THEN [k \in tracked |-> live[k]] ELSE snapshot
  /\ UNCHANGED <<lifecycle, stage, tracked, live, snapshotOrdinal,
                 publishedOrdinal, cycle, captureBefore, applyBefore,
                 publicationBefore, lastApplyAccepted, lastApplyFailed,
                 captureBackendFailed>>

CaptureBackendFailed ==
  /\ lifecycle = "Idle" /\ stage = "MutatedBeforeCapture"
  /\ StateBlockMatches("Idle", "CaptureBackendFailed", "Poisoned", "FailStop",
                       "Preserve", "Preserve", "Preserve")
  /\ StateBlockValueMatches("CaptureBackendFailed", "PoisonFailStop",
                            TRUE, FALSE, FALSE, TRUE)
  /\ lifecycle' = "Poisoned" /\ stage' = "Poisoned"
  /\ lastCaptureFailed' = TRUE /\ captureBackendFailed' = TRUE
  /\ UNCHANGED <<tracked, live, snapshot, snapshotOrdinal, publishedOrdinal,
                 cycle, captureBefore, applyBefore, publicationBefore,
                 lastApplyAccepted, lastApplyFailed>>

MutateAfterCapture ==
  /\ lifecycle = "Idle" /\ stage = "Captured"
  /\ live' = [k \in Keys |-> NextValue(live[k])]
  /\ stage' = "MutatedAfterCapture" /\ lastApplyAccepted' = FALSE
  /\ lastApplyFailed' = FALSE
  /\ UNCHANGED <<lifecycle, tracked, snapshot, snapshotOrdinal,
                 publishedOrdinal, cycle, captureBefore, applyBefore,
                 publicationBefore, lastCaptureFailed,
                 captureBackendFailed>>

ApplyPreEffectFailed ==
  /\ lifecycle = "Idle"
  /\ stage \in {"Captured", "MutatedAfterCapture"}
  /\ StateBlockMatches("Idle", "ApplyPrepareFailed", "Idle", "Preserve",
                       "Preserve", "Preserve", "Preserve")
  /\ StateBlockValueMatches("ApplyPreEffectFailed", "Preserve",
                            TRUE, FALSE, FALSE, FALSE)
  /\ UNCHANGED vars

ApplyPrepared ==
  /\ lifecycle = "Idle"
  /\ stage \in {"Captured", "MutatedAfterCapture"}
  /\ StateBlockMatches("Idle", "ApplyPrepared", "ApplyPrepared",
                       "RetainApplyRefs", "Preserve", "Retain", "Preserve")
  /\ StateBlockValueMatches("ApplyPrepared", "Preserve",
                            TRUE, FALSE, FALSE, FALSE)
  /\ lifecycle' = "ApplyPrepared" /\ stage' = "ApplyPrepared"
  /\ applyBefore' = live /\ publicationBefore' = publishedOrdinal
  /\ lastApplyAccepted' = FALSE /\ lastApplyFailed' = FALSE
  /\ UNCHANGED <<tracked, live, snapshot, snapshotOrdinal,
                 publishedOrdinal, cycle, captureBefore, lastCaptureFailed,
                 captureBackendFailed>>

ApplyAccepted ==
  /\ lifecycle = "ApplyPrepared" /\ stage = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendAccepted", "Idle",
                       "TransferApplyRefs", "Preserve", "Transfer", "Preserve")
  /\ StateBlockValueMatches("ApplyAccepted", "PublishApply",
                            TRUE, FALSE, TRUE, FALSE)
  /\ lifecycle' = "Idle" /\ stage' = "Idle" /\ cycle' = cycle + 1
  /\ live' = IF Mutation = "StaleApply" THEN live
              ELSE [k \in Keys |-> IF k \in tracked
                                      THEN snapshot[k] ELSE live[k]]
  /\ publishedOrdinal' = IF Mutation = "StaleApply" THEN publishedOrdinal
                          ELSE snapshotOrdinal
  /\ lastApplyAccepted' = TRUE /\ lastApplyFailed' = FALSE
  /\ lastCaptureFailed' = FALSE
  /\ UNCHANGED <<tracked, snapshot, snapshotOrdinal, captureBefore,
                 applyBefore, publicationBefore, captureBackendFailed>>

ApplyBackendFailed ==
  /\ lifecycle = "ApplyPrepared" /\ stage = "ApplyPrepared"
  /\ StateBlockMatches("ApplyPrepared", "ApplyBackendFailed", "Poisoned",
                       "FailStop", "Preserve", "Release", "Preserve")
  /\ StateBlockValueMatches("ApplyBackendFailed", "PoisonFailStop",
                            TRUE, FALSE, FALSE, TRUE)
  /\ lifecycle' = "Poisoned" /\ stage' = "Poisoned"
  /\ lastApplyFailed' = TRUE /\ lastApplyAccepted' = FALSE
  /\ UNCHANGED <<tracked, live, snapshot, snapshotOrdinal, publishedOrdinal,
                 cycle, captureBefore, applyBefore, publicationBefore,
                 lastCaptureFailed, captureBackendFailed>>

Next == MutateBeforeCapture \/ CaptureAccepted \/ CapturePreEffectFailed \/
        CaptureBackendFailed \/ MutateAfterCapture \/ ApplyPreEffectFailed \/
        ApplyPrepared \/ ApplyAccepted \/ ApplyBackendFailed

TypeOK == lifecycle \in {"Idle", "ApplyPrepared", "Poisoned"} /\
          stage \in {"Idle", "MutatedBeforeCapture", "Captured",
                    "MutatedAfterCapture", "ApplyPrepared", "Poisoned"} /\
          cycle \in 0..MaxCycles
FrozenTrackedSet == tracked = FrozenTracked
FailedCapturePreservesSnapshot ==
  lastCaptureFailed => snapshot = captureBefore
CaptureBackendFailStop == captureBackendFailed => lifecycle = "Poisoned"
UntrackedSamplerIsolation ==
  DOMAIN snapshot = tracked /\
  (lastApplyAccepted => live["S"] = applyBefore["S"])
LatestCapturedApplied ==
  lastApplyAccepted =>
    /\ \A k \in tracked : live[k] = snapshot[k]
    /\ publishedOrdinal = snapshotOrdinal
FailedApplyPreservesPublication ==
  lastApplyFailed => publishedOrdinal = publicationBefore
PostEntryApplyFailStop == lastApplyFailed => lifecycle = "Poisoned"

Spec == Init /\ [][Next]_vars
====
