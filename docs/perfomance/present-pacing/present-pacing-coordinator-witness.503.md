---
domain: present-pacing
workload: STALKER CoP late Normal capture, offline failure attribution
title: "Coordinator rejection now retains a bounded native witness; live attribution remains pending"
type: evidence
status: failure-witness-tested-live-capture-pending
updated: 2026-10-13
source: traces/stkcop-coordinator-witness-h503/{console-preflight.json,native-final.txt,build-unix.txt,build-x86.txt,build-x64.txt,receipt.json}; tests/native/bridge/{render_tape_identity_v4_spec.cpp,render_tape_capture_spec.cpp}
related: present-pacing-scene-request-preflight.502.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Preserve the distinguishing rejection before another live capture

H502 progressed beyond scene selection and request/arm, but its
`InvalidCoordinator` status could originate in three different layers:
canonical record facts, V4 transaction/attempt matching, or shared coordinator
provenance validation. That result does not identify a faulty runtime operation
or justify changing admission. The prior goal turn is progress: it narrowed the
next action to this actual capture rejection, not another performance A/B.

`TapeCoordinatorFailure` now carries the first rejecting branch, original
coordinator index and canonical record fact through the existing cold validator
calls. `RenderTapeCaptureSession` retains that value after terminal payload
release and clears it at a new attachment attempt or successful arm. It contains
no borrowed payload, reconstructed success receipt, or timing authority.

| Reason | Distinguishes |
|---|---|
| `CanonicalControl` | Canonical ordered-control construction failed |
| `QueryCycle` | Query identity/cycle advancement while deriving facts failed |
| `AttemptJoin` | Coordinator has no matching admitted committed transaction |
| `CoordinatorTable` | Invalid borrowed coordinator table adapter |
| `DispositionMembership` | Missing, duplicate or invalid coordinator membership |
| `ShapeOrRecord` | Coordinator shape, record mapping or release action mismatch |
| `QueryIdentity` | Observed query fields differ from the canonical fact |
| `UnusedCoordinator` | Complete stream leaves an unreferenced coordinator |

On PE attachment failure, the logger emits the witness, capture identity and
baseline, the selected coordinator, at most four containing/matching attempt
rows, and at most one referenced source and initializer. Candidate count and
truncation are explicit. These are failure-only diagnostics, not a new dump of
all draws or a published replay bundle. A truncated witness cannot be claimed
to reproduce the complete rejected identity.

No admission predicate, queue transition, query behavior, wire format, source
order or execution policy changes. The optional witness is confined to cold
capture/identity validation; normal per-draw paths gain no observer payload.
The formal layer is the existing validator contract: this diagnostic-only
change does not introduce a new scheduling/lifetime refinement. Native tests
compare witness-enabled and witness-disabled status and serialized output.

## Verification and remaining boundary

Eight V4 fixture variants cover valid, missing attempt join, invalid coordinator
shape and query-ordinal mismatch under both OrdinarySerial and SessionRelease.
The actual capture-owner fixture proves the witness survives abort and is
cleared by rearm. Focused identity V3/V4, V4 transfer, capture and install-name
tests pass 5/5; both PE builds and the full Unix build pass.

The console preflight reports both `IOConsoleLocked=true` and a locked active
user session. No game, runtime staging or GPU capture was started in this step.
The next permitted live attempt keeps H502's spatial selector and uses these
diagnostics to identify the actual rejecting coordinator/attempt. Then reduce
that evidence into a native counterexample before changing behavior.

There is still no new authenticated late-scene input, GPU original-image oracle,
largest-default-thread CPU attribution or correctness-qualified removable cost.
H477's mixed Primary attribution remains open; this instrumentation does not
replace that goal with capture success or local Encode savings.
