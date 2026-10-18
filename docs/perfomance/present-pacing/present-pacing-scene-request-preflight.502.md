---
domain: present-pacing
workload: STALKER CoP late camera-envelope Normal capture
title: "Late-scene requests arm correctly; capture attachment rejects coordinator provenance"
type: evidence
status: live-request-accepted-identity-attachment-rejected
updated: 2026-10-12
source: traces/stkcop-scene-request-h502/{selection.py,test_selection.py,selection-tests.json,run.py,console_state.py,test_console_state.py,console-tests.txt,analyze.py,capture-tests.txt}; traces/stkcop-scene-request-h502/r1/preflight.json; traces/stkcop-scene-request-h502/{r2,r3}/{runtime-preflight.json,receipt.json,analysis.json,request.bin,runtime-log.txt,progress.jsonl,supervisor.py,selection.py,console_state.py,restore/inventory.json,restore/restored.json}; traces/stkcop-root-game-h477/default-r1/{analysis.json,progress.jsonl}; traces/stkcop-normal-finish-h481/progress.jsonl
related: present-pacing-arm-request.501.md; present-pacing-tail-input-scope.500.md; present-pacing-root-game.477.md
---

# Spatial selection before another capture

H501's production selector is now paired with a bounded experiment supervisor.
The predeclared trigger is the second camera knot of H477 default-r1's retained
late path: position (-25.503149, 13.656160, 75.482338), with its retained view
direction. A coherent active-game observation must be within 1 m / 1.5 degrees
before the supervisor issues the one-shot request. It is not triggered by a
new guess at elapsed time or Present count.

After requesting, observed qualified cameras must remain within 2 m / 3 degrees
of the piecewise-linear corridor through the seven retained late camera knots.
This corridor is an explicit coarse scene envelope, **not** exact captured-draw
pose, cross-run workload equality or an interpolated CPU measurement.

## Offline evidence

The production supervisor's `selection.py` finds 13 matching stable observations
in H477 default-r1 and none in the earlier H481 capture run. This confirms the
selector discriminates these two retained inputs; repeated observations of the
same frame are not independent samples. Five synthetic counterexamples reject:
missing non-running bracket, overlapping observation envelopes, running reset,
frame regression and camera escape after requesting.

The supervisor uses the ordinary catalogue launcher, authenticates all five
renderer images, retains the original shared Wine files and benchmark results,
and checks guest PID against the PE ready receipt before invoking the H501
writer. It records request, raw observer brackets and published manifest hash.
It has positive launch/run/helper deadlines and identity-checked termination of
its owned engine. The two live failure-cleanup paths now pass; publication
cleanup remains unexercised.
Offline source/topology/image authentication is still required after capture;
`scene_qualified` and `timing_qualified` remain false in its receipts.

## Initial external gate and console correction

Xcode 27, Metal compiler and Apple M1/Metal 4 doctor checks succeed. There is
more than 6 GiB free, game/renderer/reference hashes match, the prefix's Z: drive
is verified and no engine is running. However, the authoritative console state
reported `CGSSessionScreenIsLocked=Yes`, and the display was asleep.

`r1/preflight.json` records `session_unlocked=false` and `launched=false`.
No game, GPU capture, profiler, staging or runtime-file replacement occurs.
The supervisor will recheck lock state before launch and request; it must not
bypass the lock or reinterpret a locked run as valid tail evidence.

On October 12 the per-session lock key was absent, as is normal on unlock.
The original text predicate wrongly treated this as locked. The supervisor now
requires a parsed `IOConsoleLocked=false` root and exactly one logged-in,
on-console session for the current UID, with no affirmative per-session lock.
Missing/malformed console state and command errors still reject. Four offline
test groups pass. This is not an unconditional absent-key bypass.

## Two bounded live attempts

Both runs use the same spatial trigger/corridor without changing tolerances.
H501's request is accepted and reaches a real token/epoch/queue arm; both stop
at final identity attachment, before any manifest is published.

| Field | r2 | r3 |
|---|---|---|
| Observed trigger outer frame / completed samples | 1,651 / 1,562 | 1,661 / 1,572 |
| Writer issue to successful arm | 475 ms | 431 ms |
| Armed token / epoch / queue | 1 / 1 / 2 | 1 / 1 / 2 |
| Stable post-request observations / unique frames | 130 / 4 | 134 / 4 |
| Final result | attachment `ValidationFailed` | attachment `ValidationFailed`, identity `InvalidCoordinator` |
| Published manifests | 0 | 0 |
| Restored shared Wine images | 5/5 | 5/5 |

The actual request brackets are 109.430–109.488 s and 111.866–111.927 s
after the observed running transition, respectively. These numbers describe
these perturbed runs only. The spatial trigger is not a claim that the live
clock reproduces H477's 103.333-second knot. All stable post-request camera
observations remain in the predeclared corridor; repeated observations of four
frames are not 130/134 independent frames.

The Metal toolchain doctor passed. Computer Use inspected the loading window
and requested foreground restoration before the running episode; the subsequent
active-game observer confirms progress. r2 used a separate UI action; r3 records
it as `startup_focus_requested`. Neither run is an FPS sample. Both preserve
their exact launch-script/selector hashes, request packet, process creation
identity, loaded-image map and failure log. No broad Wine/process kill was used.

r2 only logged capture status 7. Before r3, the PE cold failure log was extended
to expose the already-retained identity and topology validation statuses:

```text
phase=attach status=7 identity_status=14 topology_status=2
```

This means `RenderTapeCaptureStatus::ValidationFailed`,
`RenderTapeIdentityV4Status::InvalidCoordinator`, and topology's initial
`InvalidHeader` value. **It does not establish an independently invalid
topology**: identity construction returns before topology construction. The
size query, snapshot copy, transfer decoding and action-witness gates already
succeeded. Both PE builds pass, Unix is unchanged, and the focused capture and
arm-request native tests pass 2/2. No admission rule was relaxed.

## Next distinguishing evidence

The remaining fault is inside V4 coordinator provenance admission, not request
expiry, file transport, blob capacity or failure to select the scene. The
current status does not distinguish `coordinatorAttemptAdmitted`'s matching
committed transaction/edge constraints from the shared provenance validator's
coordinator shape, record or query-prefix constraints. Do not pick one by guess.
Before another live retry, retain a bounded rejected-input diagnostic or a
precise failing coordinator/attempt witness and reduce it to a native fixture.
Keep rejected evidence separate from an authenticated publication; do not weaken
the validator or move the selector until an easier frame happens to pass.

Source/topology authentication, original-image replay and default-tail CPU
attribution remain open. No new CPU owner, removable amount, FPS value or
default-tail transfer is established here.
