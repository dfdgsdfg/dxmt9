---
domain: present-pacing
workload: STALKER CoP late Normal capture
title: "Late capture loses closed occlusion query absolute Issue history"
type: evidence
status: native-counterexample-confirmed-fix-pending
updated: 2026-10-13
source: traces/stkcop-scene-request-h502/r4/{receipt.json,analysis.json,runtime-log.txt,supervisor.py,selection.py,console_state.py,restore/restored.json,native-counterexample.txt}; tests/native/bridge/render_tape_identity_v4_spec.cpp
related: present-pacing-coordinator-witness.503.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# The rejecting branch is QueryIdentity, not an attempt join

H502/r4 runs the H503 diagnostic images after an unlocked-console preflight.
Xcode/Metal doctor passes. The existing spatial selector fires at frame 1594,
completed count 1505; the request arms 533 ms after issue. The external camera
bracket supplies 160 stable post-request observations across four frames.
No FPS claim follows: this is a capture diagnostic, not an observer-OFF run.

Final identity attachment rejects `InvalidCoordinator`, reason `QueryIdentity`.
Coordinator index 1, event 4082, record 0, raw 366323, attempt generation 18
has the following independently compared values:

| Field | Canonical tape fact | Runtime coordinator |
|---|---:|---:|
| query kind/generation/object | 5 / 1 / 4294967407 | 5 / 1 / 4294967407 |
| query type / flags | 9 / 2 (occlusion BEGIN) | 9 / 2 |
| issue cycle | 509 | 509 |
| absolute Issue ordinal | 1 | 1017 |

The matching committed attempt covers records [0, 21), with edges
begin 76, effect 77, ordinary admission 78, dispatch 79, terminal 103.
The attempt join is not the failing predicate. The topology status remains its
initial value because identity validation fails before topology construction.

## Loss boundary and deterministic reduction

`D3D9DeviceImpl::AppendQueryIssueForChild` maintains both cycle and ordinal in
the capture-only registry. Its inactive-capture descriptor update serializes
`RenderTapeQueryDescriptorV2`, which contains `initialIssueCycle` but no initial
Issue ordinal. `renderTapeInitialQueryCycle` seeds the V2 ordinal to zero.
Consequently `buildTapeCanonicalFacts` derives ordinal 1 for the next BEGIN,
while `RenderTapeRuntimeIdentityCore::queryIssue` records the real Unix ordinal
1017. The completed-EVENT V3 checkpoint carries this information, but only for
EVENT queries; it does not fix closed occlusion history.

`closedQueryOrdinalLossCounterexample` reduces the observed mismatch to a
closed V2 cycle 508 followed by a captured BEGIN/END. Structural transcript
validation succeeds; real ordinals 1017/1018 then correctly fail identity
validation against canonical 1/2. The focused x86_64 native target passes.
This is a distinguishing reduced fixture, not a reconstruction of all rejected
runtime rows. No admission condition or production query behavior changed.

The attempted host `build/` regeneration first failed its SDK/stdlib probe
(`libc++` and `stdio.h` unavailable to that invocation). The verified test uses
the existing canonical x86_64 build with an explicit Xcode SDKROOT; it does not
claim host-target coverage.

## Required repair and remaining goal

The next repair must preserve the registry's independently maintained initial
ordinal in an explicit versioned closed-query descriptor. Do not infer it from
cycle, reset the runtime ordinal, or weaken equality. This descriptor is history
provenance only: it must grant no old-result/old-END completion authority, and
must not admit an open occlusion prefix. Replay must separately map captured
absolute history to a fresh local query and require a new captured BEGIN/END
before observing an occlusion result. Current V4 execution preparation also
rejects closed initial state, so repairing capture alone is insufficient.

Required negatives include wrong ordinal, wrong identity/generation, overflow,
old-cycle polls and missing captured BEGIN. Preserve the existing V2 meaning
and completed-EVENT checkpoint contract. Native mapping tests and a new live
late-source capture/original-image oracle remain necessary.

The supervisor restores all five shared runtime images, and independent
post-run hashes match. No owned engine/helper remains, no manifest was
published, and no rejected bytes were promoted to authenticated input.
The largest default-tail single-thread CPU owner and removable cost remain
unproven; H477's mixed Primary attribution is still open.
