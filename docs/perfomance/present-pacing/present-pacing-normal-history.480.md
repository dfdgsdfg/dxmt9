---
domain: present-pacing
workload: STALKER CoP Day Normal-action capture diagnostic
title: "Normal attachment history implemented; query and finish gates block fresh capture"
type: evidence
status: implementation-tested-wild-capture-rejected
updated: 2026-10-11
source: traces/stkcop-normal-history-h480/{run.py,analyze.py,summary.json,receipt.json,loaded-images.txt,progress.jsonl,restore/restored.json}; traces/stkcop-normal-history-h480/verification/{native-testlog.txt,x86_64-testlog.txt}; experiments/output/app-d3d9-stkcop-bench-normal-history-h480/{result.json,xrEngine_dxmt9.log}
related: present-pacing-normal-capture.479.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Fresh capture reaches query and identity-finish rejection gates

`ffa5e754` implements R-HARN-REPLAY-9.20: a cold retained surface census,
quiescent queue observation, immutable arm-time history, versioned transfer,
atomic publication and digest-bound inspection. History records original
color-touch/current-backbuffer/discard state and logical alias identities.
It explicitly supplies **no defined-content or replay authority**. The native
tests cover the actual arm, transfer and publisher path; they do not establish
actual-game publication or original-versus-replay pixel equality.

## Finite live diagnostic

H480 uses the H479-r2 settings: Sikarugir CX24.0.7, Day/high/1280x720,
`-max-threads 2`, 16 audio voices, Metal occlusion OFF, draw-command limit 0,
product counters/frame sampling/validation OFF, capture-only Normal actions,
frame-tape skip 1,400, 1 GiB blob limit, 65,536 events and 16,384 blob entries.
The receipt pins all five staged image hashes and the modified source hashes;
the process started before the implementation commit, so its recorded basis
HEAD is `1a1cfb81`, not a claim to run that commit's unmodified sources.

The first interval arms successfully, then rejects:

| Field | Observed value |
|---|---|
| Reason | `query_uncaptured_cycle` |
| Wire identity | kind 5, generation 1, object 4294967407 |
| Query type | 9 (`OCCLUSION`) |
| Initial observed cycle | 376 |
| Capture membership | inactive, not captured |
| Poll | `FLUSH`, `S_OK`, output present, size/defined bytes 4 |

The logged output **shape is not the numeric result**; no sample count can be
inferred from it. Metal occlusion OFF is not permission to invent a count or
convert an occlusion query to EVENT. Subsequent arm attempts encounter later
cycles of the same identity. The current automatic retry path repeats the
large snapshot/readback preparation at later Presents. This is diagnostic
perturbation, not an ordinary-rendering performance regression or an FPS
measurement.

There are 43 completed arms: 20 intervals reject at the producer's uncaptured
cycle guard; another 6 fail final transcript validation with the same
`query-uncaptured-cycle` classification (identities 4294967410, 4294967415,
4294967411 and 4294967409). Sixteen intervals reach `identity_v4_finish` and
fail without a stage-specific diagnostic. The final arm has no completed
disposition before the runner's timeout. Its process elapsed time is 604.27 s:
the launcher applies its 480 s wait **after** the 120 s screenshot delay.
The process exit code is 143 and the supervisor returns 1. The summary retains
the full attempt accounting, not only the first failure.

No complete bundle is published, so actual history rows, manifest bindings and
source/output pixels cannot be qualified from this run. Accessibility window
reads also failed; no visual-correctness claim is made. Five original shared
Wine images are restored and hash-checked only after the owned engine and
observer exit. The catalogue also flags a black image, but window capture
failed and fell back to a full-screen image; that flag does not independently
establish a game-rendering regression.

## Production guard and next scope

`d3d9_pe_device_tape_child.cpp` /
`NotifyRenderTapeOrderedControlForChild` requires captured issue membership or
an arm-local completed EVENT checkpoint for an old-cycle poll.
`d3d9_pe_device_tape.cpp` / `snapshotRenderTapeResourcesAtArmBoundary` only constructs that
checkpoint for type 8. The Unix provider's
`dxmt9c_device_capture_render_tape_event_query_checkpoint` independently rejects
other query types. The query rejection therefore follows the existing contract,
R-HARN-REPLAY-9.2 and R-HARN-REPLAY-9.9. It does not explain the 16 finish
failures or exonerate history serialization. The deterministic
`queryTranscriptV2PreservesCallsAndRejectsStaleCycles` test already pins a closed
pre-cut occlusion result's rejection and a new fully captured cycle's acceptance.

The follow-up change separates cold finish failures into binding/domain,
drain/completion, observer removal, freeze/snapshot, action witness and transfer
stages, retaining token/epoch/queue/Present and root first-failure provenance.
PE also distinguishes size-query, copy, decode, witness and final attachment
failure. No timing or logging is added to normal draw/replay paths. Native
tests inject valid-format history with wrong epoch, queue or baseline into the
actual frozen binding: no success receipt is exposed, and restoring the
original bytes permits the same-Present retry without resampling policy.
This instrumentation was added **after** H480; it cannot retroactively locate
that run's finish failures.

Follow-up verification passes 6/6 native and 7/7 x86_64 focused tests (including
the Unix install-name audit). Both PE builds and the Unix provider rebuild.
The three actual corrupted-history finish calls report `phase=transfer`,
`detail=3` (`CaptureMismatch`), with an unchanged frozen witness and no receipt.
The follow-up only separates existing short-circuit checks and logs rejected
cold operations; it changes no rendering, queue or lifetime transition. The
previous 230-configuration TLA regression remains applicable, not proof of
Normal pixels or an extension of query checkpoint admission.

Before another wild attempt, first isolate that finish stage and bound the
retry count. The separate old-cycle strategy needs an explicit choice:

1. A new, explicitly versioned completed-occlusion checkpoint would need the
   exact current generation/cycle/Issue ordinal, device incarnation, published
   END and completion authority **plus independently obtained defined count**.
   Pending/open/stale/reset cases must remain rejected. A logged `S_OK` alone
   is insufficient.
2. Restoring an initial numeric result must not fabricate contributing draws,
   relabel captured queue sequences as local completion, or qualify it as a
   freshly replayed GPU occlusion oracle. Its setup lifetime and replacement
   BEGIN/END behavior need explicit model/native binding before implementation.
3. Alternatively, a longer input must actually contain the contributing
   BEGIN/draw/END history. Merely moving the skip until a frame happens to
   publish does not establish general late-tail closure.

Separately, bound automatic retries so one unsupported interval cannot repeat
expensive arm snapshots until the outer watchdog ends the run. Do not weaken
the current query guard to obtain a history file.

Normal defined-content admission/restoration and the original GPU oracle still
follow this capture gate. H477's largest default CPU owner and removable-cost
question remains unanswered; neither the completed implementation nor this
rejection establishes CPU savings.
